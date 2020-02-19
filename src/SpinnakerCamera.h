#pragma once
#include "cinder/app/App.h"
#include "cinder/app/RendererGl.h"
#include "cinder/gl/gl.h"
#include "cinder/params/Params.h"
#include "cinder/Thread.h"
#include "cinder/ConcurrentCircularBuffer.h"

#include "Spinnaker.h"
#include "SpinGenApi/SpinnakerGenApi.h"

#include "CameraParams.h"
#include "CinderOpenCv.h"

using namespace ci;
using namespace ci::app;
using namespace std;

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using namespace Spinnaker::GenICam;


//cv::Mat tempDepth = cv::Mat(device_height, device_width, CV_16UC1, (void*)m_depthFrame.getData());


class SpinnakerCamera;
typedef std::shared_ptr<SpinnakerCamera> SpinnakerCameraRef;

class SpinnakerCamera {
public:

	static SpinnakerCameraRef create(SystemPtr _system, int index, params::InterfaceGlRef _paramGUI);

	SpinnakerCamera(SystemPtr system, int index, params::InterfaceGlRef paramGUI);
	~SpinnakerCamera();

	gl::TextureRef getLatestCameraTexture();
	cv::Mat getLatestCameraMat();

	float getFps();
	int getLatestDroppedFrames();

	bool checkCameraStreaming();

	string getSerialNumber();
	void cleanup();
	void printInfo();

private:
	SystemPtr system;
	int cameraIndex = -1;
	params::InterfaceGlRef paramGUI;

	CameraPtr camera = NULL;
	gl::TextureRef cameraTexture = NULL;
	cv::Mat cameraMatrix;

	double lastDroppedFramesTime = 0;
	atomic<int> droppedFrames = 0;

	double prevFrameTime = 0;
	atomic<float> fps = 0;

	CameraParams cameraParams;

	bool checkCameraAssigned();

	bool checkCameraInitialized();
	bool cameraInitialized = false;
	double lastCameraInitFailTime = -100;

	void checkParamInterfaceInitialized();
	bool paramInterfaceInitialized = false;

	bool checkCameraStopped();
	bool cameraStreaming = false; // cache of camera streaming state
	
	int prevCaptureWidth = 0;
	int prevCaptureHeight = 0;

	shared_ptr<thread> captureThread;
	ConcurrentCircularBuffer<gl::TextureRef> *frameBuffer;
	ConcurrentCircularBuffer<cv::Mat> *cvFrameBuffer;

	void captureThreadFn(gl::ContextRef sharedGlContext);
	bool shouldQuit = false;

	gl::TextureRef latestTexture = NULL;
	cv::Mat mLatestMat;


	bool checkCameraUpdatedAndRunning();
	gl::TextureRef getNextCameraTexture(); // also makes user camera is initialized. blocks during camera initialization and until next texture is received

	void save();
};