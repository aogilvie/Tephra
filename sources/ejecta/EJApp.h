/****************************************************************************

****************************************************************************/

#ifndef __EJ_APP_H__
#define __EJ_APP_H__

#ifdef _WINDOWS
#include <windows.h>
#include <tchar.h>
#include <gl/glew.h>
#include <gl/gl.h>
#else
#include <jni.h>
#include <android/log.h>
#include <GLES2/gl2.h>
#endif

#include <string>
#include <set>
#include <JavaScriptCore/JavaScriptCore.h>

#include "EJCocoa/support/nsMacros.h"
#include "EJCocoa/NSDictionary.h"
#include "EJCocoa/NSObject.h"
#include "EJCocoa/NSString.h"
#include "EJCocoa/NSSet.h"
#include "EJCocoa/NSValue.h"

#include "EJSharedOpenGLContext.h"

using namespace std;

#define EJECTA_VERSION "0.99"
#define EJECTA_APP_FOLDER "cache/"

class EJBindingBase;
class EJTimerCollection;
class EJCanvasContext;
class EJCanvasContextScreen;

class EJBindingTouchInput;


class EJApp : public NSObject {

private:
	BOOL paused;

	JavaVM *jvm;
	jobject g_obj;
        
	NSDictionary * jsClasses;
	EJTimerCollection * timers;
	long currentTime;

	EJSharedOpenGLContext *openGLContext;

	static EJApp* ejectaInstance;

	char* mainBundle;

	
public:

	BOOL landscapeMode;
	JSGlobalContextRef jsGlobalContext;
	int height, width;

	EJBindingTouchInput * touchDelegate;
	EJCanvasContext * currentRenderingContext;
	EJCanvasContextScreen * screenRenderingContext;
	float internalScaling;
	BOOL lockTouches;
	NSArray* touches;

    EJApp(void);
    ~EJApp(void);

    void init(JNIEnv* env, jobject jobj, const char* path, int w, int h);
    void setScreenSize(int w, int h);
    void run(void);
    void pause(void);
    void resume(void);
    void clearCaches(void);
    NSString * pathForResource(NSString * resourcePath);
    JSValueRef createTimer(JSContextRef ctx, size_t argc, const JSValueRef argv[], BOOL repeat);
    JSValueRef deleteTimer(JSContextRef ctx, size_t argc, const JSValueRef argv[]);

    JSClassRef getJSClassForClass(EJBindingBase* classId);
    void hideLoadingScreen(void);
    void loadJavaScriptFile(const char *filename);
    void loadScriptAtPath(NSString * path);
    JSValueRef loadModuleWithId(NSString * moduleId, JSValueRef module, JSValueRef exports);
    JSValueRef invokeCallback(JSObjectRef callback, JSObjectRef thisObject, size_t argc, const JSValueRef argv[]);
    void logException(JSValueRef exception, JSContextRef ctxp);

    void touchesBegan(int x, int y);
    void touchesEnded(int x, int y);
    void touchesCancelled(int x, int y);
    void touchesMoved(int x, int y);

    static EJApp* instance();
    static void finalize();
    void setCurrentRenderingContext(EJCanvasContext * renderingContext);

	EJSharedOpenGLContext *getOpenGLContext() const { return openGLContext; }

};

#endif // __EJ_APP_H__
