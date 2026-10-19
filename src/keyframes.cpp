#include "OsirisEdit.hpp"
#include "keyframes.hpp"

#include <string.h>

static void selection(int &start, int &end) {
	start = mini(selectedId, lastSelectedId);
	end = maxi(selectedId, lastSelectedId);
}

std::vector<int> editorKeyframes() {
	int start, end;
	selection(start, end);
	return wt::keyframesFor(BANK_LEN, currentBank.keyframes, start, end);
}

std::vector<float> editorWavesAsHeard() {
	std::vector<float> s((size_t)BANK_LEN * WAVE_LEN);
	for (int w = 0; w < BANK_LEN; w++)
		memcpy(&s[(size_t)w * WAVE_LEN], currentBank.waves[w].postSamples, sizeof(float) * WAVE_LEN);
	return s;
}

std::vector<float> editorKeyframePreview(wt::MorphMode mode, wt::Curve c) {
	std::vector<float> s = editorWavesAsHeard();
	wt::morphKeyframes(s.data(), BANK_LEN, editorKeyframes(), mode, c);
	return s;
}

int editorKeyframeMorph(wt::MorphMode mode, wt::Curve c) {
	std::vector<int> keys = editorKeyframes();
	std::vector<wt::Segment> segments = wt::segmentsFor(keys);
	if (segments.empty())
		return 0;
	std::vector<float> s = editorWavesAsHeard();
	int written = wt::morphKeyframes(s.data(), BANK_LEN, keys, mode, c);
	for (const wt::Segment &seg : segments) {
		for (int w = seg.from + 1; w < seg.to; w++) {
			Wave &wave = currentBank.waves[w];
			memcpy(wave.samples, &s[(size_t)w * WAVE_LEN], sizeof(float) * WAVE_LEN);
			memset(wave.effects, 0, sizeof(wave.effects));
			wave.cycle = false;
			wave.normalize = false;
			wave.commitSamples();
		}
	}
	historyPushNew();
	return written;
}

void editorToggleKeyframes() {
	int start, end;
	selection(start, end);
	bool allOn = true;
	for (int w = start; w <= end; w++)
		allOn = allOn && currentBank.keyframes[w];
	for (int w = start; w <= end; w++)
		currentBank.keyframes[w] = !allOn;
	historyPushNew();
}
