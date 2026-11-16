# dmgbuild settings for New Blitz's Mac disk image (tools/release/make_downloads.sh runs it):
#
#   .venv/bin/dmgbuild -s platform/macos/dmg_settings.py -D app="<New Blitz.app>" -D background=<background.png> \
#       "New Blitz" <out.dmg>
#
# dmgbuild writes the window's .DS_Store itself (background, icon spots, icon size, window bounds), so nothing
# scripts the Finder and no permission prompt appears. The background comes from tools/make_dmg_background.py: its
# background@2x.png beside it is joined into one HiDPI .background.tiff. Its numbers and these must agree.
# BRANDING: the Mac disk image window
import os.path

app = defines["app"]  # noqa: F821 (dmgbuild provides defines)
app_name = os.path.basename(app)

format = "ULFO"  # lzfse, macOS 10.11+
filesystem = "HFS+"
size = None
files = [app]
symlinks = {"Applications": "/Applications"}
icon = os.path.join(app, "Contents", "Resources", "AppIcon.icns")  # the volume's icon: the app's own

# Never shown, even with hidden files visible in the Finder (dot files already are hidden by default).
hide = [".background.tiff", ".VolumeIcon.icns"]


def create_hook(mount_point, options):  # noqa: ARG001
    # The volume was attached read-write while building; drop the Finder's event log and trash before it's sealed.
    import shutil

    for junk in (".fseventsd", ".Trashes"):
        shutil.rmtree(os.path.join(mount_point, junk), ignore_errors=True)


background = defines["background"]  # noqa: F821
show_status_bar = False
show_tab_view = False
show_toolbar = False
show_pathbar = False
show_sidebar = False
default_view = "icon-view"
show_icon_preview = False
include_icon_view_settings = True
include_list_view_settings = False

# 660x400 points of background below a 28-point title bar (the image runs on 40 points, for a shorter one).
window_rect = ((200, 140), (660, 428))
icon_size = 128
text_size = 13
label_pos = "bottom"
arrange_by = None
icon_locations = {
    app_name: (170, 192),
    "Applications": (490, 192),
}
