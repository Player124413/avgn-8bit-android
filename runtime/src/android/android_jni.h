#ifndef _BS_ANDROID_JNI_H_
#define _BS_ANDROID_JNI_H_

// Access to the Java VM from runtime code that is not the JNI host itself (src/android/main.c), e.g. the video
// backend, which drives android.media.MediaPlayer through JNI on the render thread.

#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The VM JNI_OnLoad got, or nullptr when the runtime is running outside an Android app. */
JavaVM* Android_getJavaVM(void);

#ifdef __cplusplus
}
#endif

#endif /* _BS_ANDROID_JNI_H_ */
