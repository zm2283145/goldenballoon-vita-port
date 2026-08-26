// file_dialog_mac.mm — NSOpenPanel. See file_dialog.h.
//
// A path returned from NSOpenPanel is TCC-exempt because the user selected it
// personally, so this reaches Documents/Downloads/an external drive with no
// permission prompt — unlike the directory scan it replaces.
#include "file_dialog.h"

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace filedialog {

bool isAvailable() { return true; }

bool openRom(std::string &out) {
    @autoreleasepool {
        // The panel must be driven on the main thread; the launcher's UI loop is
        // the main thread, so this is a straight call rather than a dispatch.
        if (![NSThread isMainThread]) {
            return false;
        }

        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.title = @"Choose a Nintendo 64 ROM";
        panel.message = @"Select your own copy of the original game (.z64, .n64 or .v64).";
        panel.prompt = @"Choose";
        panel.allowsMultipleSelection = NO;
        panel.canChooseDirectories = NO;
        panel.canChooseFiles = YES;
        panel.resolvesAliases = YES;
        // Let the user reach anywhere they can normally reach, including
        // external volumes where dumps commonly live.
        panel.treatsFilePackagesAsDirectories = NO;
        panel.showsHiddenFiles = NO;

        // Filter to the three N64 image extensions. Golden Balloon targets
        // macOS 13+, so use the current Uniform Type Identifiers API directly
        // and keep deprecated allowedFileTypes out of warning-clean builds.
        NSMutableArray<UTType *> *types = [NSMutableArray array];
        for (NSString *ext in @[ @"z64", @"n64", @"v64" ]) {
            UTType *type = [UTType typeWithFilenameExtension:ext];
            if (type != nil) [types addObject:type];
        }
        if (types.count > 0) {
            panel.allowedContentTypes = types;
        }
        // Never trap the user: if their dump has an unusual extension they can
        // still select it.
        panel.allowsOtherFileTypes = YES;

        // The launcher runs as a plain SDL app that may not be the active
        // application; without this the panel can open behind the game window.
        [NSApp activateIgnoringOtherApps:YES];

        NSModalResponse response = [panel runModal];
        if (response != NSModalResponseOK) {
            return false;   // user cancelled — leave `out` untouched
        }

        NSURL *url = panel.URLs.firstObject;
        if (url == nil || !url.isFileURL) {
            return false;
        }
        const char *path = url.fileSystemRepresentation;
        if (path == nullptr || path[0] == '\0') {
            return false;
        }
        out = path;
        return true;
    }
}

bool openCharacterSource(std::string &out) {
    @autoreleasepool {
        if (![NSThread isMainThread]) return false;
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.title = @"Import a custom character";
        panel.message = @"Choose a package, model, archive, or DCC source. Non-GLB authoring formats receive safe export guidance.";
        panel.prompt = @"Choose Source";
        panel.allowsMultipleSelection = NO;
        panel.canChooseDirectories = NO;
        panel.canChooseFiles = YES;
        panel.resolvesAliases = YES;
        panel.treatsFilePackagesAsDirectories = NO;
        panel.showsHiddenFiles = NO;
        NSMutableArray<UTType *> *types = [NSMutableArray array];
        for (NSString *ext in @[
                 @"mdkrchar", @"glb", @"dae", @"gltf", @"fbx", @"obj",
                 @"blend", @"usd", @"usda", @"usdc", @"usdz", @"ma",
                 @"mb", @"max", @"c4d", @"3ds"
             ]) {
            UTType *type = [UTType typeWithFilenameExtension:ext];
            if (type != nil) [types addObject:type];
        }
        [types addObject:UTTypeZIP];
        panel.allowedContentTypes = types;
        panel.allowsOtherFileTypes = NO;
        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK) return false;
        NSURL *url = panel.URLs.firstObject;
        if (url == nil || !url.isFileURL) return false;
        const char *path = url.fileSystemRepresentation;
        if (path == nullptr || path[0] == '\0') return false;
        out = path;
        return true;
    }
}

bool openCharacterLicense(std::string &out) {
    @autoreleasepool {
        if (![NSThread isMainThread]) return false;
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.title = @"Choose the character license or notice file";
        panel.message = @"Choose the exact LICENSE, COPYING, or notice text to embed in the source package.";
        panel.prompt = @"Choose License";
        panel.allowsMultipleSelection = NO;
        panel.canChooseDirectories = NO;
        panel.canChooseFiles = YES;
        panel.resolvesAliases = YES;
        panel.treatsFilePackagesAsDirectories = NO;
        panel.showsHiddenFiles = NO;
        panel.allowsOtherFileTypes = YES;
        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK) return false;
        NSURL *url = panel.URLs.firstObject;
        if (url == nil || !url.isFileURL) return false;
        const char *path = url.fileSystemRepresentation;
        if (path == nullptr || path[0] == '\0') return false;
        out = path;
        return true;
    }
}

bool openPortraitImage(std::string &out) {
    @autoreleasepool {
        if (![NSThread isMainThread]) return false;
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.title = @"Choose character portrait artwork";
        panel.message = @"Choose a square PNG (16–1024 pixels). The Workshop previews the exact 40×40 in-game result.";
        panel.prompt = @"Choose Portrait";
        panel.allowsMultipleSelection = NO;
        panel.canChooseDirectories = NO;
        panel.canChooseFiles = YES;
        panel.resolvesAliases = YES;
        panel.treatsFilePackagesAsDirectories = NO;
        panel.showsHiddenFiles = NO;
        panel.allowedContentTypes = @[ UTTypePNG ];
        panel.allowsOtherFileTypes = NO;
        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK) return false;
        NSURL *url = panel.URLs.firstObject;
        if (url == nil || !url.isFileURL) return false;
        const char *path = url.fileSystemRepresentation;
        if (path == nullptr || path[0] == '\0') return false;
        out = path;
        return true;
    }
}

bool saveCharacterConvertedGlb(std::string &out) {
    @autoreleasepool {
        if (![NSThread isMainThread]) return false;
        NSSavePanel *panel = [NSSavePanel savePanel];
        panel.title = @"Save converted character model";
        panel.message = @"Choose a new GLB filename. Golden Balloon never overwrites an existing model.";
        panel.prompt = @"Choose Filename";
        panel.nameFieldStringValue = @"converted-character.glb";
        panel.canCreateDirectories = YES;
        UTType *glbType = [UTType typeWithFilenameExtension:@"glb"];
        if (glbType != nil) panel.allowedContentTypes = @[ glbType ];
        panel.allowsOtherFileTypes = NO;
        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK) return false;
        NSURL *url = panel.URL;
        if (url == nil || !url.isFileURL) return false;
        const char *path = url.fileSystemRepresentation;
        if (path == nullptr || path[0] == '\0') return false;
        out = path;
        return true;
    }
}

bool saveCharacterCapture(std::string &out) {
    @autoreleasepool {
        if (![NSThread isMainThread]) return false;
        NSSavePanel *panel = [NSSavePanel savePanel];
        panel.title = @"Save a custom character inspection";
        panel.message = @"Choose a new PNG filename. Golden Balloon never overwrites an existing capture.";
        panel.prompt = @"Choose Filename";
        panel.nameFieldStringValue = @"character-inspection.png";
        panel.canCreateDirectories = YES;
        panel.allowedContentTypes = @[ UTTypePNG ];
        panel.allowsOtherFileTypes = NO;
        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK) return false;
        NSURL *url = panel.URL;
        if (url == nil || !url.isFileURL) return false;
        const char *path = url.fileSystemRepresentation;
        if (path == nullptr || path[0] == '\0') return false;
        out = path;
        return true;
    }
}

bool saveCharacterReport(std::string &out) {
    @autoreleasepool {
        if (![NSThread isMainThread]) return false;
        NSSavePanel *panel = [NSSavePanel savePanel];
        panel.title = @"Export custom character visual report";
        panel.message = @"Choose a new HTML filename. The report embeds its PNGs and never overwrites an existing file.";
        panel.prompt = @"Choose Filename";
        panel.nameFieldStringValue = @"character-visual-report.html";
        panel.canCreateDirectories = YES;
        panel.allowedContentTypes = @[ UTTypeHTML ];
        panel.allowsOtherFileTypes = NO;
        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK) return false;
        NSURL *url = panel.URL;
        if (url == nil || !url.isFileURL) return false;
        const char *path = url.fileSystemRepresentation;
        if (path == nullptr || path[0] == '\0') return false;
        out = path;
        return true;
    }
}

}  // namespace filedialog
