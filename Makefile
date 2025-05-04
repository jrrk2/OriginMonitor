./Release/CelestronOriginMonitor.app: ./CelestronOriginMonitor.xcodeproj/project.pbxproj
	@rm -rf Release
	xcodebuild -project CelestronOriginMonitor.xcodeproj -scheme CelestronOriginMonitor -configuration Release
	/opt/homebrew/Cellar/qt/6.9.0/bin/macdeployqt $@ -libpath=/opt/homebrew/Cellar/qt/6.9.0/lib -always-overwrite -verbose=2
	/opt/homebrew/Cellar/qt/6.9.0/bin/macdeployqt $@ -libpath=/opt/homebrew/Cellar/qt/6.9.0/lib -always-overwrite -verbose=2
	python3 fix_app_bundle.py

./CelestronOriginMonitor.xcodeproj/project.pbxproj: CelestronOriginMonitor.pro
	qmake -spec macx-xcode $<
#	sed -I .orig -e 's=Y53675G8V9=5AU5B5HJQX=g' -e 's=com.yourdomain=uk.kimmitt=g' -e 's/QT_LIBRARY_SUFFIX = ""/CODE_SIGN_IDENTITY = "Apple Distribution"/' $@

run: ./Release/CelestronOriginMonitor.app
	./Release/CelestronOriginMonitor.app/Contents/MacOS/CelestronOriginMonitor
