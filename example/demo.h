#ifndef DEMO_H
#define DEMO_H

#include "nanovg.h"

#ifdef __cplusplus
extern "C" {
#endif

struct DemoData {
	int fontNormal, fontBold, fontIcons, fontEmoji;
	int images[12];
};
typedef struct DemoData DemoData;

int loadDemoData(NVGcontext* vg, DemoData* data);
void freeDemoData(NVGcontext* vg, DemoData* data);
void renderDemo(NVGcontext* vg, float mx, float my, float width, float height, float t, int blowup, DemoData* data);

void saveScreenShot(int w, int h, int premult, const char* name);

// Image utilities
void unpremultiplyAlpha(unsigned char* image, int w, int h, int stride);
void setAlpha(unsigned char* image, int w, int h, int stride, unsigned char a);
void flipHorizontal(unsigned char* image, int w, int h, int stride);
void swapBGRA(unsigned char *imageBytes, int width, int height, int stride);

#ifdef __cplusplus
}
#endif

#endif // DEMO_H
