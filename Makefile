default: monitor

./Release/CelestronOriginMonitor.app: monitor
	xcodebuild -project CelestronOriginMonitor.xcodeproj -scheme CelestronOriginMonitor -configuration Release

monitor: CelestronOriginMonitor.pro
	qmake -spec macx-xcode $<

run: ./Release/CelestronOriginMonitor.app
	./Release/CelestronOriginMonitor.app/Contents/MacOS/CelestronOriginMonitor
