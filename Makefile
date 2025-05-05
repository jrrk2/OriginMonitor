# Makefile for building, signing, and notarizing CelestronOriginMonitor.app

# Variables
PROJECT = CelestronOriginMonitor.xcodeproj
SCHEME = CelestronOriginMonitor
ARCHIVE_PATH = build/CelestronOriginMonitor.xcarchive
EXPORT_PATH = build/exported
EXPORT_OPTIONS_PLIST = exportOptions.plist
QT_LIB_PATH = /opt/homebrew/Cellar/qt/6.9.0/lib
DEVELOPER_ID = "Developer ID Application: Mr Jonathan Kimmitt (Y53675G8V9)"
ENTITLEMENTS = sandbox.entitlements
BUILD_DIR=build

# Clean up previous builds
clean:
	@rm -rf $(BUILD_DIR)/Release
	@rm -rf $(BUILD_DIR)/Celestron*
	@rm -rf $(BUILD_DIR)/exported
	@rm -rf $(BUILD_DIR)/obj
	@echo "Cleaned up previous build artifacts, preserving necessary files like build/moc/"

# Build the archive
build/CelestronOriginMonitor.xcarchive:
	xcodebuild archive \
		-project $(PROJECT) \
		-scheme $(SCHEME) \
		-archivePath $(ARCHIVE_PATH) \
		CODE_SIGN_IDENTITY=$(DEVELOPER_ID) \
		CODE_SIGN_ENTITLEMENTS=$(ENTITLEMENTS) \
		ENABLE_HARDENED_RUNTIME=YES \
		OTHER_CODE_SIGN_FLAGS="--options runtime"
	@echo "Archive created at $(ARCHIVE_PATH)."

# Export the app and deploy Qt libraries
build/exported/CelestronOriginMonitor.app: build/CelestronOriginMonitor.xcarchive
	# Export the app (unsigned at this point)
	xcodebuild -exportArchive \
		-archivePath $< \
		-exportPath $(EXPORT_PATH) \
		-exportOptionsPlist $(EXPORT_OPTIONS_PLIST)

	# Deploy Qt libraries
	/opt/homebrew/Cellar/qt/6.9.0/bin/macdeployqt $(EXPORT_PATH)/CelestronOriginMonitor.app \
		-libpath=$(QT_LIB_PATH) -always-overwrite -verbose=2

	# Deploy Qt libraries
	/opt/homebrew/Cellar/qt/6.9.0/bin/macdeployqt $(EXPORT_PATH)/CelestronOriginMonitor.app \
		-libpath=$(QT_LIB_PATH) -always-overwrite -verbose=2

	# Fix app bundle if needed
	python3 fix_app_bundle.py $(EXPORT_PATH)/CelestronOriginMonitor.app

	find $@ -type f \( -name "*.dylib" -o -perm +111 \) -exec codesign --force --timestamp --options runtime --sign $(DEVELOPER_ID) {} \;

	# Re-sign the app after modification
	codesign --force --deep --options runtime --timestamp \
		--sign $(DEVELOPER_ID) \
		$(EXPORT_PATH)/CelestronOriginMonitor.app
	@echo "App signed and ready for distribution."

# Create the .zip file for distribution
build/exported/CelestronOriginMonitor.zip: build/exported/CelestronOriginMonitor.app
	(cd $(EXPORT_PATH); ditto -c -k --keepParent CelestronOriginMonitor.app CelestronOriginMonitor.zip)
	@echo "App zipped for distribution at $(EXPORT_PATH)/CelestronOriginMonitor.zip"

# Notarize the app
notary: build/exported/CelestronOriginMonitor.zip
	(cd $(EXPORT_PATH); xcrun notarytool submit CelestronOriginMonitor.zip \
		--apple-id "jonathan@kimmitt.co.uk" \
		--team-id Y53675G8V9 \
		--keychain-profile "notary-profile-name" \
		--wait)
	@echo "Notarization complete."

# Staple the notarization ticket
staple: build/exported/CelestronOriginMonitor.app
	xcrun stapler staple $(EXPORT_PATH)/CelestronOriginMonitor.app
	@echo "Stapling notarization ticket to app."

# Verify the notarized app
verify: build/exported/CelestronOriginMonitor.app
	spctl --assess --type execute --verbose $(EXPORT_PATH)/CelestronOriginMonitor.app
	@echo "App verification complete."

# Build and run the app locally (during development)
./Release/CelestronOriginMonitor.app: ./CelestronOriginMonitor.xcodeproj/project.pbxproj
	@rm -rf Release
	xcodebuild -project $(PROJECT) -scheme $(SCHEME) -configuration Release
	/opt/homebrew/Cellar/qt/6.9.0/bin/macdeployqt $@ -libpath=$(QT_LIB_PATH) -always-overwrite -verbose=2
	/opt/homebrew/Cellar/qt/6.9.0/bin/macdeployqt $@ -libpath=$(QT_LIB_PATH) -always-overwrite -verbose=2
	python3 fix_app_bundle.py

run: ./Release/CelestronOriginMonitor.app
	./Release/CelestronOriginMonitor.app/Contents/MacOS/CelestronOriginMonitor

debug:
	xcrun notarytool log $(NOTARYID) --apple-id "jonathan@kimmitt.co.uk" --team-id Y53675G8V9 --keychain-profile "notary-profile-name"

open: build/exported/CelestronOriginMonitor.app
	open build/exported/CelestronOriginMonitor.app
