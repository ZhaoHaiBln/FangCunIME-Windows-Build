//go:build !windows

package main

func broadcastSettingsChanged() {}

func isSystemDarkMode() bool {
	return false
}
