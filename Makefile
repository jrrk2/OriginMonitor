./Release/CelestronOriginMonitor.app: monitor
	@rm -rf Release
	xcodebuild -project CelestronOriginMonitor.xcodeproj -scheme CelestronOriginMonitor -configuration Release
	/opt/homebrew/Cellar/qt/6.9.0/bin/macdeployqt $@ -libpath=/opt/homebrew/Cellar/qt/6.9.0/lib -always-overwrite -verbose=2
	/opt/homebrew/Cellar/qt/6.9.0/bin/macdeployqt $@ -libpath=/opt/homebrew/Cellar/qt/6.9.0/lib -always-overwrite -verbose=2
	python3 fix_app_bundle.py

monitor: CelestronOriginMonitor.pro
	qmake -spec macx-xcode $<

run: ./Release/CelestronOriginMonitor.app
	./Release/CelestronOriginMonitor.app/Contents/MacOS/CelestronOriginMonitor
