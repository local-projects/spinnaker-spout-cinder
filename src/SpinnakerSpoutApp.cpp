#include "SpinnakerSpoutApp.h"

using namespace ci;
using namespace ci::app;
using namespace std;

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using namespace Spinnaker::GenICam;

#define CAMERA_AVAILABLE_CHECK_INTERVAL 2

/*
Make sure to enable jumbo packets on your network interface and set them to 9014 bytes:
https://www.ptgrey.com/KB/10899#Jumbo%20Packet (assuming you have a network interface that supports this)

When working with multiple camera's on a network with limited bandwidth, make sure they distribute the bandwidth equally using the Device Link Throughput Limit setting.

In general, keep these guidelines in mind:
https://www.ptgrey.com/KB/11151
*/

void prepareSettings(App::Settings *settings)
{
	settings->setHighDensityDisplayEnabled();
	settings->setTitle("Spinnaker to Spout");
	settings->setWindowSize(1024, 768);
	settings->setFullScreen(false);
	settings->setResizable(true);
	settings->setFrameRate(60.0f);
}

void SpinnakerSpoutApp::setup()
{
	sendWidth = UserSettings::getSetting<int>("SendWidth", sendWidth);
	sendHeight = UserSettings::getSetting<int>("SendHeight", sendHeight);
	binning = UserSettings::getSetting<int>("Binning", binning);
	exposure = UserSettings::getSetting<double>("ExposureTimeAbs", exposure);
	logLevelIndex = UserSettings::getSetting<int>("LogLevelIndex", logLevelIndex);
	deviceLinkThroughputLimit = UserSettings::getSetting<int>("DeviceLinkThroughputLimit", deviceLinkThroughputLimit);

	gl::enableAlphaBlending();
	gl::color(ColorA(1, 1, 1, 1));
}

void SpinnakerSpoutApp::draw()
{
	gl::clear(ColorA(0, 0, 0, 1));
	gl::setMatricesWindow(getWindowSize());

	string status;
	int fps = 0;

	if (needsInitText) {
		// draw this before starting camera initialization because it blocks app
		status = "Initializing...";
		needsInitText = false;
	}
	else {
		updateCameraTexture(status); // blocks during intializion of the camera and every frame until a new texture arrived 

		if (cameraTexture != NULL) {
			if (paramGUI == NULL) initParamInterface(); // requires an active camera
			updateMovingCameraParams();

			bool flip = true;
			gl::draw(cameraTexture, Rectf(0.f, flip ? (float)getWindowHeight() : 0.f, (float)getWindowWidth(), flip ? 0.f : (float)getWindowHeight()));

			sendToSpout(cameraTexture);
			fps = (int)getAverageFps();
		}
	}

	float fontSize = (float)(getWindowWidth() / 50);

	float fpsLeft = getWindowWidth() - toPixels(240);
	float fpsTop = toPixels(20);

	float infoLeft = toPixels(20);
	float infoTop = getWindowHeight() - toPixels(20) - fontSize;

	{
		gl::ScopedColor colorScope(ColorA(0, 0, 0, 0.5));
		gl::drawSolidRect(Rectf(fpsLeft - 10.f, fpsTop - 10.f, (float)getWindowWidth(), fpsTop + fontSize + 10.f));
		gl::drawSolidRect(Rectf(0.f, infoTop - 10.f, (float)getWindowWidth(), infoTop + fontSize + 10.f));
	}

	gl::drawString(status, vec2(infoLeft, infoTop), ColorA(1, 1, 1, 1), Font("Verdana", fontSize));

	stringstream ss;
	ss << "Fps: " << fps << ", dropped " << geLatestDroppedFrames();
	gl::drawString(ss.str(), vec2(fpsLeft, fpsTop), ColorA(1, 1, 1, 1), Font("Verdana", fontSize));

	if (paramGUI != NULL) paramGUI->draw();
}

void SpinnakerSpoutApp::sendToSpout(gl::TextureRef &sendTexture) {
	if (!checkSpoutInitialized()) return;

	gl::ScopedFramebuffer frameBufferScope(sendFbo);
	gl::ScopedViewport viewPortScope(sendFbo->getSize());
	gl::ScopedMatrices matrixScope;
	gl::setMatricesWindow(sendFbo->getSize(), false);

	gl::draw(sendTexture, sendFbo->getBounds());

	// Send the texture for all receivers to use
	// NOTE : if SendTexture is called with a framebuffer object bound,
	// include the FBO id as an argument so that the binding is restored afterwards
	// because Spout uses an fbo for intermediate rendering
	auto tex = sendFbo->getColorTexture();
	spoutSender.SendTexture(tex->getId(), tex->getTarget(), tex->getWidth(), tex->getHeight());
}

bool spoutInitialized = false;
bool SpinnakerSpoutApp::checkSpoutInitialized() {
	if (spoutInitialized && sendFbo != NULL && sendFbo->getWidth() == sendWidth && sendFbo->getHeight() == sendHeight) return true;

	if (spoutInitialized) spoutSender.ReleaseSender();

	sendFbo = gl::Fbo::create(sendWidth, sendHeight);
	spoutInitialized = spoutSender.CreateSender(senderName.c_str(), sendWidth, sendHeight);

	// MemoryShareMode informs us whether Spout is initialized for texture share or for memory share
	bool memoryMode = spoutSender.GetMemoryShareMode();
	if (spoutInitialized) console() << "Spout initialized using " << (memoryMode ? "Memory" : "Texture") << " sharing at " << sendWidth << " x " << sendHeight << endl;
	else console() << "Spout initialization failed" << endl;

	return spoutInitialized;
}

void SpinnakerSpoutApp::initParamInterface() {
	paramGUI = params::InterfaceGl::create(getWindow(), "Parameters", toPixels(ivec2(200, 300)));

	paramGUI->addParam("Camera Log Level", SpinnakerDeviceCommunication::getLogLevelStrings(), &logLevelIndex).min(20).updateFn([this] {
		system->SetLoggingEventPriorityLevel(indexToSpinnakerLogLevel(logLevelIndex));
		UserSettings::writeSetting<int>("LogLevelIndex", logLevelIndex);
	});

	paramGUI->addSeparator();

	std::vector<string> binningEnums;
	binningEnums.push_back("No binning");
	binningEnums.push_back("Binning 2x");
	paramGUI->addParam("Binning", binningEnums, &binning).updateFn([this] {
		cameraSettingsDirty = true;
		UserSettings::writeSetting<int>("Binning", binning);
	});

	CameraParam::createEnum("Gain Auto", "GainAuto", paramGUI, camera, 0);
	CameraParam::createEnum("White Balance Auto", "BalanceWhiteAuto", paramGUI, camera, 0);

	paramGUI->addParam("White Balance Ratio", &balanceRatio).updateFn([this] {
		cameraSettingsDirty = true;
		UserSettings::writeSetting<double>("BalanceRatio", balanceRatio);
	});

	CameraParam::createEnum("Exposure Auto", "ExposureAuto", paramGUI, camera, 0);

	paramGUI->addParam("Exposure", &exposure).updateFn([this] {
		cameraSettingsDirty = true;
		UserSettings::writeSetting<double>("ExposureTimeAbs", exposure);
	});

	CameraParam::createEnum("Pixel Format", "PixelFormat", paramGUI, camera, 4, true);

	paramGUI->addSeparator("Stream");

	paramGUI->addParam("Device Link Throughput Limit", &deviceLinkThroughputLimit).updateFn([this] {
		cameraSettingsDirty = true;
		UserSettings::writeSetting<int>("DeviceLinkThroughputLimit", deviceLinkThroughputLimit);
	});

	paramGUI->addSeparator("Spout");

	// -------- SPOUT --------
	paramGUI->addParam("Send Width", &sendWidth).min(20).updateFn([this] {
		UserSettings::writeSetting<int>("SendWidth", sendWidth);
	});

	paramGUI->addParam("Send Height", &sendHeight).min(20).updateFn([this] {
		UserSettings::writeSetting<int>("SendHeight", sendHeight);
	});
}

void SpinnakerSpoutApp::updateCameraTexture(string &status) {
	if (!checkCameraInitialized()) { // blocks while initializing
		status = "No camera available.";
		cameraTexture = NULL;
		return;
	}

	if (camera != NULL) CameraParam::applyParams();  // potentially stops camera acquisition to apply changed settings

	if (cameraSettingsDirty) {
		cameraSettingsDirty = false;
		if (camera != NULL) applyCameraSettings(); // potentially stops camera acquisition to apply changed settings
	}

	if (!SpinnakerDeviceCommunication::checkStreamingStarted(camera)) {
		status = "Unable to start camera...";
		cameraTexture = NULL;
		return;
	}

	if (!camera->IsValid()) {
		SpinnakerDeviceCommunication::checkStreamingStopped(camera);
		camera->DeInit();
		status = "Camera status invalid. Attempting to restart.";
		console() << "Camera status invalid." << endl;
		cameraTexture = NULL;
		return;
	}

	bool success = SpinnakerDeviceCommunication::getCameraTexture(camera, cameraTexture); // block until a new frame is available or a frame is reported incomplete. (re-)initializes output texture if needed

	if (success == false || cameraTexture == NULL) {
		status = "Dropped frame.";
		droppedFrames++;
		return;
	}

	stringstream ss;
	ss << "Capturing from " << camera->DeviceModelName.GetValue() << " at " << cameraTexture->getWidth() << " x " << cameraTexture->getHeight() << ", sending as " << senderName.c_str() << " at " << sendWidth << " x " << sendHeight;
	status = ss.str();
}

// Stops camera if necessary. Make sure to check whether it is started after calling this method.
// returns true if camera was stopped, false otherwise
bool SpinnakerSpoutApp::applyCameraSettings() {
	bool cameraWasStopped = false;
	// API is weird here since you can only read binning from Horizontal but need to write to Horizontal and Vertical
	if (SpinnakerDeviceCommunication::getParameterIntValue(camera, "BinningHorizontal") - 1 != binning) {
		cameraWasStopped = SpinnakerDeviceCommunication::checkStreamingStopped(camera);
		SpinnakerDeviceCommunication::setParameterInt(camera, "BinningHorizontal", binning + 1); // binning param values are set as int starting from 1
		SpinnakerDeviceCommunication::setParameterInt(camera, "BinningVertical", binning + 1);
	}

	if (SpinnakerDeviceCommunication::getParameterFloatValue(camera, "BalanceRatio") != balanceRatio) {
		balanceRatio = SpinnakerDeviceCommunication::setParameterFloat(camera, "BalanceRatio", balanceRatio);
	}

	if (SpinnakerDeviceCommunication::getParameterFloatValue(camera, "ExposureTimeAbs") != exposure) {
		exposure = SpinnakerDeviceCommunication::setParameterFloat(camera, "ExposureTimeAbs", exposure);
	}

	if (SpinnakerDeviceCommunication::getParameterIntValue(camera, "DeviceLinkThroughputLimit") != deviceLinkThroughputLimit) {
		int newValue = SpinnakerDeviceCommunication::setParameterInt(camera, "DeviceLinkThroughputLimit", deviceLinkThroughputLimit);
		if (newValue != deviceLinkThroughputLimit) {
			deviceLinkThroughputLimit = newValue;
			UserSettings::writeSetting<int>("DeviceLinkThroughputLimit", newValue);
		}
	}

	return cameraWasStopped;
}

void SpinnakerSpoutApp::updateMovingCameraParams() {
	double newExposure = SpinnakerDeviceCommunication::getParameterFloatValue(camera, "ExposureTimeAbs");
	if (newExposure != exposure) {
		exposure = newExposure;
		UserSettings::writeSetting<double>("ExposureTimeAbs", exposure);
	}

	double newBalanceRatio = SpinnakerDeviceCommunication::getParameterFloatValue(camera, "BalanceRatio");
	if (newBalanceRatio != balanceRatio) {
		balanceRatio = newBalanceRatio;
		UserSettings::writeSetting<double>("BalanceRatio", balanceRatio);
	}
}

double lastCameraInitCheckTime = -100;
bool SpinnakerSpoutApp::checkCameraInitialized() {
	if (camera != NULL && camera->IsInitialized()) return true;

	if (getElapsedSeconds() - lastCameraInitCheckTime < CAMERA_AVAILABLE_CHECK_INTERVAL) return false;
	lastCameraInitCheckTime = getElapsedSeconds();

	try {
		if (system == NULL) {
			system = System::GetInstance();

			const LibraryVersion spinnakerLibraryVersion = system->GetLibraryVersion();
			console() << "Spinnaker library version: "
				<< spinnakerLibraryVersion.major << "."
				<< spinnakerLibraryVersion.minor << "."
				<< spinnakerLibraryVersion.type << "."
				<< spinnakerLibraryVersion.build << endl << endl;

			system->RegisterLoggingEvent(loggingEventHandler);
			system->SetLoggingEventPriorityLevel(indexToSpinnakerLogLevel(logLevelIndex));
		}

		CameraList camList = system->GetCameras();

		unsigned int numCameras = camList.GetSize();
		if (numCameras == 0)
		{
			console() << "No cameras found" << endl;
		}
		else {
			camera = camList.GetByIndex(0);
			try {
				console() << "Initializing camera 0 (" << camList.GetSize() << " available)..." << endl;
				camera->Init();
				//SpinnakerDeviceCommunication::printDeviceInfo(camera);
				return true;
			}
			catch (Spinnaker::Exception &e) {
				console() << "Error initializing camera: " << e.what() << endl;
			}
		}
	}
	catch (exception e) {
		console() << "Error initializing Spinnaker library: " << e.what() << ". Do the included Spinnaker header and lib files match the dll version?" << endl;
	}

	console() << "Initializing camera failed, retrying in " << CAMERA_AVAILABLE_CHECK_INTERVAL << " seconds." << endl;

	return false;
}

double lastDroppedFramesTime = 0;
int SpinnakerSpoutApp::geLatestDroppedFrames() {
	if (getElapsedSeconds() - lastDroppedFramesTime > 1) {
		droppedFrames = 0;
		lastDroppedFramesTime = getElapsedSeconds();
	}
	return droppedFrames;
}

void SpinnakerSpoutApp::cleanup()
{
	if (camera != NULL) {
		if (camera->IsStreaming()) camera->EndAcquisition();
		if (camera->IsInitialized()) camera->DeInit();
		camera = NULL;
	}
	system->UnregisterLoggingEvent(loggingEventHandler);
	system->ReleaseInstance(); // Release system
	spoutSender.ReleaseSender();
}

SpinnakerLogLevel indexToSpinnakerLogLevel(int index) {
	switch (index) {
		case 0: return LOG_LEVEL_OFF;
		case 1: return LOG_LEVEL_FATAL;
		case 2: return LOG_LEVEL_ALERT;
		case 3: return LOG_LEVEL_CRIT;
		case 4: return LOG_LEVEL_ERROR;
		case 5: return LOG_LEVEL_WARN;
		case 6: return LOG_LEVEL_NOTICE;
		case 7: return LOG_LEVEL_INFO;
		case 8: return LOG_LEVEL_DEBUG;
		case 9: return LOG_LEVEL_NOTSET;
		default: return LOG_LEVEL_OFF;
	}
}

CINDER_APP(SpinnakerSpoutApp, RendererGl, prepareSettings)
