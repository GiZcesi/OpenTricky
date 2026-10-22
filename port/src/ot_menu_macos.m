/* The macOS menu bar entries: "Settings…" (Cmd+,) in the app menu, and a "Game" menu (full screen, window sizes, screenshot, folders).
 * The items post SDL user events (code = OT_MENU_*), which the window's event loop handles on the same thread as the other keys. */
#import <Cocoa/Cocoa.h>
#include <SDL3/SDL.h>

enum { OT_MENU_SETTINGS = 1, OT_MENU_FULLSCREEN, OT_MENU_SCREENSHOT, OT_MENU_SMALLER, OT_MENU_LARGER,
       OT_MENU_SIZE_540 = 10, OT_MENU_SIZE_720, OT_MENU_SIZE_900, OT_MENU_SIZE_1080 };

@interface OTMenuTarget : NSObject
- (void)post:(NSMenuItem *)item;
- (void)showFolder:(NSMenuItem *)item;
@end

@implementation OTMenuTarget
- (void)post:(NSMenuItem *)item
{
    SDL_Event ev; SDL_zero(ev);
    ev.type = SDL_EVENT_USER;
    ev.user.code = (Sint32)item.tag;
    SDL_PushEvent(&ev);
}
- (void)showFolder:(NSMenuItem *)item
{
    NSString *dir = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/OpenTricky"];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:dir]];
}
@end

static OTMenuTarget *s_target;

static NSMenuItem *item(NSString *title, NSString *key, NSEventModifierFlags mods, NSInteger tag, SEL action)
{
    NSMenuItem *m = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
    m.target = s_target; m.tag = tag; m.keyEquivalentModifierMask = mods;
    return m;
}

void ot_menu_install(void)
{
    @autoreleasepool {
        NSMenu *main = NSApp.mainMenu;
        if (!main || s_target) return;
        s_target = [OTMenuTarget new];

        NSMenu *app = [main itemAtIndex:0].submenu;       /* SDL's application menu: About / Services / Hide / Quit */
        if (app) {
            /* SDL3 already puts a "Settings…" item in the application menu (inactive: it has no action). Wire it up; add one if it is not there. */
            NSMenuItem *found = nil;
            for (NSMenuItem *m in app.itemArray)
                if ([m.title hasPrefix:@"Settings"] || [m.title hasPrefix:@"Preferences"]) { found = m; break; }
            if (found) {
                found.target = s_target; found.action = @selector(post:); found.tag = OT_MENU_SETTINGS;
                found.keyEquivalent = @","; found.keyEquivalentModifierMask = NSEventModifierFlagCommand; found.enabled = YES;
            } else {
                NSInteger at = app.numberOfItems > 1 ? 1 : app.numberOfItems;
                [app insertItem:[NSMenuItem separatorItem] atIndex:at];
                [app insertItem:item(@"Settings…", @",", NSEventModifierFlagCommand, OT_MENU_SETTINGS, @selector(post:)) atIndex:at];
            }
        }

        NSMenu *game = [[NSMenu alloc] initWithTitle:@"Game"];
        [game addItem:item(@"Settings…", @"", 0, OT_MENU_SETTINGS, @selector(post:))];
        [game addItem:[NSMenuItem separatorItem]];
        [game addItem:item(@"Enter or Leave Full Screen", @"f", NSEventModifierFlagCommand | NSEventModifierFlagControl, OT_MENU_FULLSCREEN, @selector(post:))];
        NSMenu *sizes = [[NSMenu alloc] initWithTitle:@"Window Size"];
        [sizes addItem:item(@"Small (540)", @"1", NSEventModifierFlagCommand, OT_MENU_SIZE_540, @selector(post:))];
        [sizes addItem:item(@"Medium (720)", @"2", NSEventModifierFlagCommand, OT_MENU_SIZE_720, @selector(post:))];
        [sizes addItem:item(@"Large (900)", @"3", NSEventModifierFlagCommand, OT_MENU_SIZE_900, @selector(post:))];
        [sizes addItem:item(@"Extra Large (1080)", @"4", NSEventModifierFlagCommand, OT_MENU_SIZE_1080, @selector(post:))];
        [sizes addItem:[NSMenuItem separatorItem]];
        [sizes addItem:item(@"Smaller", @"-", NSEventModifierFlagCommand, OT_MENU_SMALLER, @selector(post:))];
        [sizes addItem:item(@"Larger", @"=", NSEventModifierFlagCommand, OT_MENU_LARGER, @selector(post:))];
        NSMenuItem *sz = [[NSMenuItem alloc] initWithTitle:@"Window Size" action:nil keyEquivalent:@""];
        sz.submenu = sizes;
        [game addItem:sz];
        [game addItem:item(@"Save Screenshot", @"", 0, OT_MENU_SCREENSHOT, @selector(post:))];
        [game addItem:[NSMenuItem separatorItem]];
        NSMenuItem *folder = [[NSMenuItem alloc] initWithTitle:@"Show Settings and Saves Folder" action:@selector(showFolder:) keyEquivalent:@""];
        folder.target = s_target;
        [game addItem:folder];

        NSMenuItem *top = [[NSMenuItem alloc] initWithTitle:@"Game" action:nil keyEquivalent:@""];
        top.submenu = game;
        [main insertItem:top atIndex:main.numberOfItems > 1 ? 1 : main.numberOfItems];
    }
}
