PLStartMarekPCStartHeader
FORMAT = PurpleLib/2.0
NAME = KustomGUI
NAMESPACE = Kustom.GUI
VERSION = 1.2.0
TYPE = Purple Lib
LANGUAGE = Purplecode
DESCRIPTION = Kustom GUI helper library with executable Purplecode bodies
AUTHOR = Purplecode Examples
LICENSE = MIT
MIN_PURPLECODE = 0.7.0
ABI_VERSION = 2
CONTROLANG = Kustom.GUI.MakeWindow
CONTROLANGS = Kustom.GUI,Kustom.Layout,Kustom.Events
EXPORTS = Kustom.GUI.MakeWindow
CAPABILITIES = gui
FEATURES = native-window,events,custom-controlangs
ICON_PATH = cache/demo.png
ICON_VECTOR = <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><rect width="64" height="64" rx="12" fill="#8048d8"/><path d="M16 18h32v28H16z" fill="#fff"/></svg>
PLEndMarekPCEndHeader

// Executable Purplecode body; imports compile the function implementation.
fn Kustom.GUI.MakeWindow(title) {
    return GUI.NativeWindow(title)
}
