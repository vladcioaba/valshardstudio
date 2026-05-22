// macOS native menu bar + file dialogs (NSOpenPanel / NSSavePanel).
//
// GLFW creates a basic application menu (Quit / About) but no File menu; we
// inject one ourselves. The menu-item target is a small Objective-C object
// whose callbacks bridge into the user-provided std::function set at
// install-time.

#import <Cocoa/Cocoa.h>

#include "NativeMenu.h"

#include "GLFW/glfw3.h"

#include <memory>

namespace {
    std::unique_ptr<ocs::MacMenuCallbacks> gCallbacks;
}

@interface OCSMenuTarget : NSObject<NSMenuDelegate>
- (void)newProject:(id)sender;
- (void)openDocument:(id)sender;
- (void)saveDocument:(id)sender;
- (void)saveDocumentAs:(id)sender;
- (void)exportCsb:(id)sender;
- (void)togglePanel:(id)sender;
- (void)detachPanel:(id)sender;
- (void)resetLayout:(id)sender;
- (void)reattachToParent:(id)sender;
- (void)openPreferences:(id)sender;
@end

@implementation OCSMenuTarget

- (void)newProject:(id)sender
{
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.allowsMultipleSelection = NO;
    panel.canChooseDirectories = YES;
    panel.canCreateDirectories = YES;
    panel.canChooseFiles = NO;
    panel.prompt = @"Create Project Here";
    if ([panel runModal] == NSModalResponseOK)
    {
        NSString* dir = panel.URLs.firstObject.path;
        if (dir && gCallbacks && gCallbacks->onNewProject)
        {
            gCallbacks->onNewProject(std::string(dir.UTF8String));
        }
    }
}

- (void)openDocument:(id)sender
{
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.allowsMultipleSelection = NO;
    panel.canChooseDirectories = NO;
    panel.canChooseFiles = YES;
    panel.allowedFileTypes = @[@"csd"];
    if ([panel runModal] == NSModalResponseOK)
    {
        NSString* path = panel.URLs.firstObject.path;
        if (path && gCallbacks && gCallbacks->onOpen)
        {
            gCallbacks->onOpen(std::string(path.UTF8String));
        }
    }
}

- (void)saveDocument:(id)sender
{
    if (gCallbacks && gCallbacks->onSave) gCallbacks->onSave();
}

- (void)saveDocumentAs:(id)sender
{
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.allowedFileTypes = @[@"csd"];
    if ([panel runModal] == NSModalResponseOK)
    {
        NSString* path = panel.URL.path;
        if (path && gCallbacks && gCallbacks->onSaveAs)
        {
            gCallbacks->onSaveAs(std::string(path.UTF8String));
        }
    }
}

- (void)exportCsb:(id)sender
{
    if (gCallbacks && gCallbacks->onExportCsb)
        gCallbacks->onExportCsb();
}

- (void)togglePanel:(id)sender
{
    NSMenuItem* item = (NSMenuItem*)sender;
    if (gCallbacks && gCallbacks->onTogglePanel)
        gCallbacks->onTogglePanel((int)item.tag);
    // Reflect the new state immediately in the checkmark — the next
    // menuNeedsUpdate will re-sync anyway, but this avoids a visible
    // lag if the menu stays open via a submenu chain.
    if (gCallbacks && gCallbacks->isPanelVisible)
        item.state = gCallbacks->isPanelVisible((int)item.tag)
            ? NSControlStateValueOn : NSControlStateValueOff;
}

- (void)detachPanel:(id)sender
{
    NSMenuItem* item = (NSMenuItem*)sender;
    if (gCallbacks && gCallbacks->onDetachPanel)
        gCallbacks->onDetachPanel((int)item.tag);
}

- (void)resetLayout:(id)sender
{
    if (gCallbacks && gCallbacks->onResetLayout)
        gCallbacks->onResetLayout();
}

- (void)reattachToParent:(id)sender
{
    if (gCallbacks && gCallbacks->onReattachToParent)
        gCallbacks->onReattachToParent();
}

- (void)openPreferences:(id)sender
{
    if (gCallbacks && gCallbacks->onOpenPreferences)
        gCallbacks->onOpenPreferences();
}

/// NSMenuDelegate — called just before the menu displays. Walk the
/// item list and sync each checkmark to the editor's current flag.
- (void)menuNeedsUpdate:(NSMenu*)menu
{
    if (!gCallbacks || !gCallbacks->isPanelVisible) return;
    for (NSMenuItem* item in menu.itemArray)
    {
        if (item.action != @selector(togglePanel:)) continue;
        item.state = gCallbacks->isPanelVisible((int)item.tag)
            ? NSControlStateValueOn : NSControlStateValueOff;
    }
}

@end

namespace ocs {

void installMacMenu(const MacMenuCallbacks& cb)
{
    gCallbacks = std::make_unique<MacMenuCallbacks>(cb);

    // Run on the main thread — the app may still be initializing when
    // AppDelegate::applicationDidFinishLaunching calls us.
    dispatch_async(dispatch_get_main_queue(), ^{
        static OCSMenuTarget* target = [[OCSMenuTarget alloc] init];

        NSMenu* mainMenu = [NSApp mainMenu];
        if (!mainMenu)
        {
            mainMenu = [[NSMenu alloc] init];
            [NSApp setMainMenu:mainMenu];
        }

        // Remove any existing "File" menu so repeated calls replace it.
        for (NSMenuItem* item in [mainMenu itemArray])
        {
            if ([item.title isEqualToString:@"File"])
            {
                [mainMenu removeItem:item];
                break;
            }
        }

        NSMenuItem* fileItem = [[NSMenuItem alloc] init];
        NSMenu* fileMenu = [[NSMenu alloc] initWithTitle:@"File"];

        NSMenuItem* newItem = [[NSMenuItem alloc]
            initWithTitle:@"New Project…"
                   action:@selector(newProject:)
            keyEquivalent:@"n"];
        newItem.target = target;
        [fileMenu addItem:newItem];

        NSMenuItem* openItem = [[NSMenuItem alloc]
            initWithTitle:@"Open…"
                   action:@selector(openDocument:)
            keyEquivalent:@"o"];
        openItem.target = target;
        [fileMenu addItem:openItem];

        [fileMenu addItem:[NSMenuItem separatorItem]];

        NSMenuItem* saveItem = [[NSMenuItem alloc]
            initWithTitle:@"Save"
                   action:@selector(saveDocument:)
            keyEquivalent:@"s"];
        saveItem.target = target;
        [fileMenu addItem:saveItem];

        NSMenuItem* saveAsItem = [[NSMenuItem alloc]
            initWithTitle:@"Save As…"
                   action:@selector(saveDocumentAs:)
            keyEquivalent:@"S"];  // shift+s
        saveAsItem.keyEquivalentModifierMask =
            NSEventModifierFlagCommand | NSEventModifierFlagShift;
        saveAsItem.target = target;
        [fileMenu addItem:saveAsItem];

        [fileMenu addItem:[NSMenuItem separatorItem]];

        NSMenuItem* exportItem = [[NSMenuItem alloc]
            initWithTitle:@"Export .csb"
                   action:@selector(exportCsb:)
            keyEquivalent:@"e"];
        exportItem.keyEquivalentModifierMask =
            NSEventModifierFlagCommand | NSEventModifierFlagShift;
        exportItem.target = target;
        [fileMenu addItem:exportItem];

        fileItem.submenu = fileMenu;

        // Insert File as the second top-level menu — after the app menu but
        // before Edit/Window/Help if GLFW added those.
        NSInteger insertAt = mainMenu.numberOfItems > 0 ? 1 : 0;
        [mainMenu insertItem:fileItem atIndex:insertAt];

        // --- View menu: panel visibility toggles. --------------------
        // Replace any existing "View" menu so repeated calls are idempotent.
        for (NSMenuItem* item in [mainMenu itemArray])
        {
            if ([item.title isEqualToString:@"View"])
            {
                [mainMenu removeItem:item];
                break;
            }
        }
        NSMenuItem* viewItem = [[NSMenuItem alloc] init];
        NSMenu* viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
        // Delegate drives the checkmark sync right before the menu shows,
        // so toggles triggered from elsewhere (panel titlebar X, keybind)
        // stay visible.
        viewMenu.delegate = target;

        NSArray<NSString*>* panelNames = @[ @"Scene", @"Assets",
                                            @"Properties", @"Layout",
                                            @"Mode Bar",
                                            @"Messages",
                                            @"Timeline",
                                            @"Sprite Editor",
                                            @"Sprite Catalog" ];
        for (NSUInteger i = 0; i < panelNames.count; ++i)
        {
            NSMenuItem* mi = [[NSMenuItem alloc]
                initWithTitle:panelNames[i]
                       action:@selector(togglePanel:)
                keyEquivalent:@""];
            mi.tag = (NSInteger)i;          // 0=Scene .. 4=Mode
            mi.target = target;
            [viewMenu addItem:mi];
        }

        // --- Detach-panel submenu ------------------------------------
        // Under View → "Detach Panel →" with the same panel list.
        // Clicking an item spawns a new OCS process in detached-panel
        // mode and hides the docked copy in the current window.
        [viewMenu addItem:[NSMenuItem separatorItem]];
        NSMenuItem* detachRoot = [[NSMenuItem alloc] init];
        detachRoot.title = @"Detach Panel";
        NSMenu* detachMenu = [[NSMenu alloc] initWithTitle:@"Detach Panel"];
        for (NSUInteger i = 0; i < panelNames.count; ++i)
        {
            NSMenuItem* mi = [[NSMenuItem alloc]
                initWithTitle:panelNames[i]
                       action:@selector(detachPanel:)
                keyEquivalent:@""];
            mi.tag = (NSInteger)i;
            mi.target = target;
            [detachMenu addItem:mi];
        }
        detachRoot.submenu = detachMenu;
        [viewMenu addItem:detachRoot];

        // --- Reset Layout (returns every panel to its default dock
        //     position and closes any detached panel windows). --------
        [viewMenu addItem:[NSMenuItem separatorItem]];
        NSMenuItem* resetItem = [[NSMenuItem alloc]
            initWithTitle:@"Reset Layout"
                   action:@selector(resetLayout:)
            keyEquivalent:@""];
        resetItem.target = target;
        [viewMenu addItem:resetItem];

        viewItem.submenu = viewMenu;
        [mainMenu insertItem:viewItem atIndex:insertAt + 1];

        // --- Preferences → Cmd+, (standard macOS shortcut). -------------
        // Added to the File menu so we don't need a whole App-menu
        // override for one item.
        [fileMenu addItem:[NSMenuItem separatorItem]];
        NSMenuItem* prefsItem = [[NSMenuItem alloc]
            initWithTitle:@"Preferences…"
                   action:@selector(openPreferences:)
            keyEquivalent:@","];
        prefsItem.target = target;
        [fileMenu addItem:prefsItem];
    });
}

void installMacMenuForDetachedPanel(const MacMenuCallbacks& cb)
{
    gCallbacks = std::make_unique<MacMenuCallbacks>(cb);
    dispatch_async(dispatch_get_main_queue(), ^{
        static OCSMenuTarget* target = [[OCSMenuTarget alloc] init];
        NSMenu* mainMenu = [NSApp mainMenu];
        if (!mainMenu)
        {
            mainMenu = [[NSMenu alloc] init];
            [NSApp setMainMenu:mainMenu];
        }
        // Scrub any previous File/View we may have had.
        for (NSMenuItem* item in [mainMenu.itemArray copy])
        {
            if ([item.title isEqualToString:@"File"] ||
                [item.title isEqualToString:@"View"])
                [mainMenu removeItem:item];
        }
        NSInteger insertAt = mainMenu.numberOfItems > 0 ? 1 : 0;
        NSMenuItem* viewItem = [[NSMenuItem alloc] init];
        NSMenu* viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
        NSMenuItem* reattachItem = [[NSMenuItem alloc]
            initWithTitle:@"Reattach to Parent Window"
                   action:@selector(reattachToParent:)
            keyEquivalent:@""];
        reattachItem.target = target;
        [viewMenu addItem:reattachItem];
        viewItem.submenu = viewMenu;
        [mainMenu insertItem:viewItem atIndex:insertAt];
    });
}

std::string showOpenImageDialog()
{
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.allowsMultipleSelection = NO;
        panel.canChooseDirectories = NO;
        panel.canChooseFiles = YES;
        panel.allowedFileTypes =
            @[@"png", @"jpg", @"jpeg", @"webp", @"bmp", @"tga"];
        if ([panel runModal] != NSModalResponseOK) return {};
        NSString* path = panel.URLs.firstObject.path;
        if (!path) return {};
        return std::string(path.UTF8String);
    }
}

void asyncSetWindowSize(void* glfwWindow, int widthPx, int heightPx)
{
    if (!glfwWindow || widthPx <= 0 || heightPx <= 0) return;
    GLFWwindow* gw = static_cast<GLFWwindow*>(glfwWindow);
    dispatch_async(dispatch_get_main_queue(), ^{
        glfwSetWindowSize(gw, widthPx, heightPx);
    });
}

std::string showChooseDirectoryDialog()
{
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.allowsMultipleSelection = NO;
        panel.canChooseDirectories = YES;
        panel.canCreateDirectories = YES;
        panel.canChooseFiles = NO;
        panel.prompt = @"Choose";
        if ([panel runModal] != NSModalResponseOK) return {};
        NSString* path = panel.URLs.firstObject.path;
        if (!path) return {};
        return std::string(path.UTF8String);
    }
}

}  // namespace ocs
