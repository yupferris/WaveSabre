#include <WaveSabrePlayerLib/SongRenderer.h>

using namespace WaveSabreCore;

namespace WaveSabrePlayerLib
{
	SongRenderer::SongRenderer(const SongRenderer::Song *song, int numRenderThreads)
	{
		Helpers::Init();

		songBlobPtr = song->blob;

		bpm = readInt();
		sampleRate = readInt();
		length = readDouble();

		numDevices = readInt();
		devices = new Device *[numDevices];
		for (int i = 0; i < numDevices; i++)
		{
			devices[i] = song->factory((DeviceId)readByte());
			devices[i]->SetSampleRate((float)sampleRate);
			devices[i]->SetTempo(bpm);
			int chunkSize = readInt();
			devices[i]->SetChunk((void *)songBlobPtr, chunkSize);
			songBlobPtr += chunkSize;
		}

		numMidiLanes = readInt();
		midiLanes = new MidiLane *[numMidiLanes];
		for (int i = 0; i < numMidiLanes; i++)
		{
			midiLanes[i] = new MidiLane;
			int numEvents = readInt();
			midiLanes[i]->numEvents = numEvents;
			midiLanes[i]->events = new Event[numEvents];
			for (int m = 0; m < numEvents; m++)
			{
				midiLanes[i]->events[m].TimeStamp = readInt();
				byte note = readByte();
				if ((note & 0x80) == 0x00)
				{
					midiLanes[i]->events[m].Type = (EventType)0;
					midiLanes[i]->events[m].Note = (note & 0x7F);
					midiLanes[i]->events[m].Velocity = readByte();
				}
				else
				{
					midiLanes[i]->events[m].Type = (EventType)1;
					midiLanes[i]->events[m].Note = (note & 0x7F);
					midiLanes[i]->events[m].Velocity = 0;
				}
			}
		}

		numTracks = readInt();
		tracks = new Track *[numTracks];
		states = new RenderState[numTracks];
		for (int i = 0; i < numTracks; i++)
		{
			tracks[i] = new Track(this, song->factory);
			states[i] = RenderState::Finished;
		}

		traceEvents = new TraceEvent[MaxTraceEvents];
		traceEventIndex = 0;

		renderSamplesCalls = 0;

		InitializeCriticalSection(&criticalSection);
		this->numRenderThreads = numRenderThreads;
		renderThreads = new HANDLE[numRenderThreads];
		shutdown = false;

		for (int i = 0; i < numRenderThreads; i++)
		{
			renderThreads[i] = CreateThread(0, 0, threadProc, (LPVOID)this, 0, 0);
			SetThreadPriority(renderThreads[i], THREAD_PRIORITY_HIGHEST);
		}

		QueryPerformanceCounter(&startCounter);
	}

	SongRenderer::~SongRenderer()
	{
		// We don't need to enter/leave a critical section here since we're the only writer at this point.
		shutdown = true;

		WaitForMultipleObjects(numRenderThreads, renderThreads, TRUE, INFINITE);
		DeleteCriticalSection(&criticalSection);

		delete [] renderThreads;

		for (int i = 0; i < numDevices; i++) delete devices[i];
		delete [] devices;

		for (int i = 0; i < numMidiLanes; i++) delete midiLanes[i];
		delete [] midiLanes;

		for (int i = 0; i < numTracks; i++) delete tracks[i];
		delete [] tracks;
		delete [] states;

		delete [] traceEvents;
	}

	void SongRenderer::RenderSamples(Sample *buffer, int numSamples)
	{
		MxcsrFlagGuard mxcsrFlagGuard;

		LARGE_INTEGER endCounter, frequency;

		QueryPerformanceCounter(&endCounter);
		QueryPerformanceFrequency(&frequency);
		auto elapsedTimeUs = (endCounter.QuadPart - startCounter.QuadPart) * 1000000 / frequency.QuadPart;
		traceEvents[traceEventIndex++] =
		{
			TraceEventType::RenderSamples,
			"Render",
			"Tracks",
			"B",
			elapsedTimeUs,
			GetCurrentProcessId(),
			GetCurrentThreadId(),
			renderSamplesCalls,
			0,
		};

		EnterCriticalSection(&criticalSection);

		for (int i = 0; i < numTracks; i++)
			states[i] = RenderState::Idle;

		numFloatSamples = numSamples / 2;

		LeaveCriticalSection(&criticalSection);

		// We don't need to enter/leave a critical section here since we're the only reader at this point.
		//  However, if we don't want to sleep, entering/leaving a critical section here also works to not
		//  starve the worker threads it seems. Sleeping feels like the better approach to reduce contention
		//  though.
		while (states[numTracks - 1] != RenderState::Finished)
			Sleep(0);

		float **masterTrackBuffers = tracks[numTracks - 1]->Buffers;
		for (int i = 0; i < numSamples; i++)
		{
			int sample = (int)(masterTrackBuffers[i & 1][i / 2] * 32767.0f);
			if (sample < -32768) sample = -32768;
			if (sample > 32767) sample = 32767;
			buffer[i] = (Sample)sample;
		}

		QueryPerformanceCounter(&endCounter);
		QueryPerformanceFrequency(&frequency);
		elapsedTimeUs = (endCounter.QuadPart - startCounter.QuadPart) * 1000000 / frequency.QuadPart;
		traceEvents[traceEventIndex++] =
		{
			TraceEventType::RenderSamples,
			"Render",
			"Tracks",
			"E",
			elapsedTimeUs,
			GetCurrentProcessId(),
			GetCurrentThreadId(),
			renderSamplesCalls,
			0,
		};

		renderSamplesCalls++;
	}

	DWORD WINAPI SongRenderer::threadProc(LPVOID lpParameter)
	{
		auto songRenderer = (SongRenderer *)lpParameter;

		MxcsrFlagGuard mxcsrFlagGuard;

		int nextTrackIndex = songRenderer->numTracks;

		bool isRunning = true;
		while (isRunning)
		{
			EnterCriticalSection(&songRenderer->criticalSection);

			if (songRenderer->shutdown)
				isRunning = false;

			LARGE_INTEGER endCounter, frequency;

			if (nextTrackIndex < songRenderer->numTracks)
			{
				QueryPerformanceCounter(&endCounter);
				QueryPerformanceFrequency(&frequency);
				auto elapsedTimeUs = (endCounter.QuadPart - songRenderer->startCounter.QuadPart) * 1000000 / frequency.QuadPart;
				songRenderer->traceEvents[songRenderer->traceEventIndex++] =
				{
					TraceEventType::RenderTrack,
					"Render",
					"Tracks",
					"E",
					elapsedTimeUs,
					GetCurrentProcessId(),
					GetCurrentThreadId(),
					0,
					nextTrackIndex,
				};

				songRenderer->states[nextTrackIndex] = RenderState::Finished;
			}

			nextTrackIndex = 0;
			for (; nextTrackIndex < songRenderer->numTracks; nextTrackIndex++)
			{
				if (songRenderer->states[nextTrackIndex] != RenderState::Idle)
					continue;

				for (int i = 0; i < songRenderer->tracks[nextTrackIndex]->numReceives; i++)
				{
					if (songRenderer->states[songRenderer->tracks[nextTrackIndex]->receives[i].SendingTrackIndex] != RenderState::Finished)
						goto cnt;
				}

				// We have a free track yeeeey
				break;

			cnt:;
			}

			if (nextTrackIndex < songRenderer->numTracks)
			{
				songRenderer->states[nextTrackIndex] = RenderState::Rendering;

				QueryPerformanceCounter(&endCounter);
				QueryPerformanceFrequency(&frequency);
				auto elapsedTimeUs = (endCounter.QuadPart - songRenderer->startCounter.QuadPart) * 1000000 / frequency.QuadPart;
				songRenderer->traceEvents[songRenderer->traceEventIndex++] =
				{
					TraceEventType::RenderTrack,
					"Render",
					"Tracks",
					"B",
					elapsedTimeUs,
					GetCurrentProcessId(),
					GetCurrentThreadId(),
					0,
					nextTrackIndex,
				};
			}

			LeaveCriticalSection(&songRenderer->criticalSection);

			if (nextTrackIndex < songRenderer->numTracks)
			{
				songRenderer->tracks[nextTrackIndex]->Run(songRenderer->numFloatSamples);
			}
			else
			{
				Sleep(0);
			}
		}

		return 0;
	}

	int SongRenderer::GetTempo() const
	{
		return bpm;
	}

	int SongRenderer::GetSampleRate() const
	{
		return sampleRate;
	}

	double SongRenderer::GetLength() const
	{
		return length;
	}

	unsigned char SongRenderer::readByte()
	{
		unsigned char ret = *songBlobPtr;
		songBlobPtr++;
		return ret;
	}

	int SongRenderer::readInt()
	{
		int ret = *(int *)songBlobPtr;
		songBlobPtr += sizeof(int);
		return ret;
	}

	float SongRenderer::readFloat()
	{
		float ret = *(float *)songBlobPtr;
		songBlobPtr += sizeof(float);
		return ret;
	}

	double SongRenderer::readDouble()
	{
		double ret = *(double *)songBlobPtr;
		songBlobPtr += sizeof(double);
		return ret;
	}
}
