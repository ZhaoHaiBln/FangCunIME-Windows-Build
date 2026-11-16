//go:build windows

package main

import (
	"syscall"
	"unsafe"
)

var (
	user32                    = syscall.NewLazyDLL("user32.dll")
	procRegisterWindowMessage = user32.NewProc("RegisterWindowMessageW")
	procFindWindowEx          = user32.NewProc("FindWindowExW")
	procPostMessage           = user32.NewProc("PostMessageW")

	advapi32            = syscall.NewLazyDLL("advapi32.dll")
	procRegOpenKeyExW   = advapi32.NewProc("RegOpenKeyExW")
	procRegQueryValueEx = advapi32.NewProc("RegQueryValueExW")
	procRegCloseKey     = advapi32.NewProc("RegCloseKey")
)

const (
	hwndMessage     = ^uintptr(2) // HWND_MESSAGE = (HWND)-3
	hkeyCurrentUser = uintptr(0x80000001)
	keyRead         = 0x20019
	regDword        = 4
)

func broadcastSettingsChanged() {
	msgName, err := syscall.UTF16PtrFromString("FangCunIME_SettingsChanged")
	if err != nil {
		return
	}
	msg, _, _ := procRegisterWindowMessage.Call(uintptr(unsafe.Pointer(msgName)))
	if msg == 0 {
		return
	}
	className, err := syscall.UTF16PtrFromString("FangCunTsfMessageWindow")
	if err != nil {
		return
	}
	var target uintptr
	for {
		next, _, _ := procFindWindowEx.Call(
			hwndMessage,
			target,
			uintptr(unsafe.Pointer(className)),
			0,
		)
		if next == 0 {
			break
		}
		procPostMessage.Call(next, msg, 0, 0)
		target = next
	}
}

func isSystemDarkMode() bool {
	subKey, err := syscall.UTF16PtrFromString(`Software\Microsoft\Windows\CurrentVersion\Themes\Personalize`)
	if err != nil {
		return false
	}
	var hKey uintptr
	r0, _, _ := procRegOpenKeyExW.Call(
		hkeyCurrentUser,
		uintptr(unsafe.Pointer(subKey)),
		0,
		keyRead,
		uintptr(unsafe.Pointer(&hKey)),
	)
	if r0 != 0 {
		return false
	}
	defer procRegCloseKey.Call(hKey)

	valName, err := syscall.UTF16PtrFromString("AppsUseLightTheme")
	if err != nil {
		return false
	}
	var valType uint32
	var val uint32 = 1
	valSize := uint32(unsafe.Sizeof(val))
	r1, _, _ := procRegQueryValueEx.Call(
		hKey,
		uintptr(unsafe.Pointer(valName)),
		0,
		uintptr(unsafe.Pointer(&valType)),
		uintptr(unsafe.Pointer(&val)),
		uintptr(unsafe.Pointer(&valSize)),
	)
	if r1 == 0 && valType == regDword {
		return val == 0
	}
	return false
}
