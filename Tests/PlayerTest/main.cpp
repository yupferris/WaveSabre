#include <WaveSabrePlayerLib.h>
using namespace WaveSabrePlayerLib;

#include <string.h>

#include "Song.h"

void progressCallback(double progress, void *data)
{
	const int barLength = 32;
	int filledChars = (int)(progress * (double)(barLength - 1));
	printf("\r[");
	for (int j = 0; j < barLength; j++) putchar(filledChars >= j ? '*' : '-');
	printf("]");
}

int main(int argc, char **argv)
{
	bool writeWav = argc >= 2 && !strcmp(argv[1], "-w");
	bool preRender = argc == 2 && !strcmp(argv[1], "-p");

	const int numRenderThreads = 3;

	if (writeWav)
	{
		WavWriter wavWriter(&Song, numRenderThreads);

		printf("WAV writer activated.\n");

		auto fileName = argc >= 3 ? argv[2] : "out.wav";
		printf("Rendering...\n");
		wavWriter.Write(fileName, progressCallback, nullptr);

		printf("\n\nWAV file written to \"%s\". Enjoy.\n", fileName);

		auto traceFileName = argv[3];
		auto file = fopen(traceFileName, "wb");

		fprintf(file, "[\n");

		for (int i = 0; i < wavWriter.songRenderer->traceEventIndex; i++)
		{
			auto e = &wavWriter.songRenderer->traceEvents[i];

			switch (e->type)
			{
			case SongRenderer::TraceEventType::RenderSamples:
				fprintf(file, "{\"name\":\"%s samples %d\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%I64d,\"pid\":%d,\"tid\":%d}%s\n", e->name, e->renderSamplesCount, e->category, e->ph, e->ts, e->pid, e->tid, i != wavWriter.songRenderer->traceEventIndex - 1 ? "," : "");
				break;
			case SongRenderer::TraceEventType::RenderTrack:
				fprintf(file, "{\"name\":\"%s track %d\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%I64d,\"pid\":%d,\"tid\":%d}%s\n", e->name, e->trackId, e->category, e->ph, e->ts, e->pid, e->tid, i != wavWriter.songRenderer->traceEventIndex - 1 ? "," : "");
				break;
			}
		}

		fprintf(file, "]\n");

		fclose(file);

		printf("\n\nTrace file written to \"%s\". Enjoy.\n", traceFileName);
	}
	else
	{
		IPlayer *player;

		if (preRender)
		{
			printf("Prerender activated.\n");
			printf("Rendering...\n");

			player = new PreRenderPlayer(&Song, numRenderThreads, progressCallback, nullptr);

			printf("\n\n");
		}
		else
		{
			player = new RealtimePlayer(&Song, numRenderThreads);
		}

		printf("Realtime player activated. Press ESC to quit.\n");
		player->Play();
		while (!GetAsyncKeyState(VK_ESCAPE))
		{
			auto songPos = player->GetSongPos();
			if (songPos >= player->GetLength()) break;
			int minutes = (int)songPos / 60;
			int seconds = (int)songPos % 60;
			int hundredths = (int)(songPos * 100.0) % 100;
			printf("\r %.1i:%.2i.%.2i", minutes, seconds, hundredths);

			Sleep(10);
		}
		printf("\n");

		delete player;
	}

	return 0;
}
