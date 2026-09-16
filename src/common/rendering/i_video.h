#ifndef __I_VIDEO_H__
#define __I_VIDEO_H__

#include <cstdint>

class DFrameBuffer;


class IVideo
{
public:
	virtual ~IVideo() {}

	virtual DFrameBuffer *CreateFrameBuffer() = 0;

	bool SetResolution();

	virtual void DumpAdapters();
};

void I_InitGraphics();
void I_ShutdownGraphics();

// True when the platform can recreate its video objects mid-session (SDL).
// When false, a backend change still requires an engine restart.
bool I_SupportsLiveBackendSwitch();

// Deletes the current IVideo and creates a new one honoring vid_preferbackend.
// Only called when I_SupportsLiveBackendSwitch() is true.
void I_RestartGraphics();

// Shows the window after the replacement framebuffer has been created.
// Only called when I_SupportsLiveBackendSwitch() is true.
void I_ShowGraphicsWindow();

// True when running without a display (-headless or BIASEDDOOM_HEADLESS=1).
// Implemented in common/rendering/nullvideo/null_video.cpp.
bool I_IsHeadless();

extern IVideo *Video;

void I_PolyPresentInit();
uint8_t *I_PolyPresentLock(int w, int h, bool vsync, int &pitch);
void I_PolyPresentUnlock(int x, int y, int w, int h);
void I_PolyPresentDeinit();


#endif // __I_VIDEO_H__
