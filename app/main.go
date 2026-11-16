package main

import (
	"log"

	"github.com/egoist/mygo"
)

// SettingsService exposes FangCun IME settings to the MyGo TypeScript frontend.
type SettingsService struct{}

// Load reads the current settings from %LOCALAPPDATA%\FangCunIME\settings.ini.
func (SettingsService) Load() AppSettings {
	return loadSettingsFile()
}

// Save writes settings.ini and notifies active FangCunTSF.dll instances immediately.
func (SettingsService) Save(s AppSettings) AppSettings {
	return saveSettingsFile(s)
}

// Reset restores default settings, saves, and notifies active FangCunTSF.dll instances.
func (SettingsService) Reset() AppSettings {
	return saveSettingsFile(defaultSettings())
}

// Close quits the settings application.
func (SettingsService) Close() {
	mygo.App.Quit()
}

func main() {
	mygo.Bind(SettingsService{})

	mygo.App.WhenReady(func() {
		mygo.NewWindow(mygo.WindowOptions{
			Title:           "方寸输入法 设置",
			Width:           920,
			Height:          700,
			MinWidth:        760,
			MinHeight:       600,
			BackgroundColor: "light-dark(#f8f6f2, #12151c)",
			StateKey:        "fangcun-settings-main",
			URL:             "/",
		})
	})

	if err := mygo.App.Run(); err != nil {
		log.Fatal(err)
	}
}
