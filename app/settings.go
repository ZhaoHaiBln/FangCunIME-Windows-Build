package main

import (
	"bufio"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// AppSettings mirrors windows/tsf/include/SettingsStore.hpp and settings.ini.
type AppSettings struct {
	Scheme             string  `json:"scheme"`             // "flypy" | "pinyin" | "ninekey"
	MixedInput         bool    `json:"mixedInput"`
	ChinesePunctuation bool    `json:"chinesePunctuation"`
	Skin               string  `json:"skin"`               // "native_light" | "warm_grid" | "mint" | "deep_blue"
	ThemeMode          string  `json:"themeMode"`          // "system" | "light" | "dark"
	DarkMode           bool    `json:"darkMode"`
	SystemDark         bool    `json:"systemDark"`
	FirstCandidateSize int     `json:"firstCandidateSize"` // 16..30
	KeyOpacity         float64 `json:"keyOpacity"`         // 0.4..1.0
	TypingGlow         bool    `json:"typingGlow"`
	KeyLetterSize      int     `json:"keyLetterSize"`      // 14..30
	KeyLetterBold      bool    `json:"keyLetterBold"`
}

func defaultSettings() AppSettings {
	s := AppSettings{
		Scheme:             "flypy",
		MixedInput:         true,
		ChinesePunctuation: true,
		Skin:               "native_light",
		ThemeMode:          "system",
		FirstCandidateSize: 20,
		KeyOpacity:         0.82,
		TypingGlow:         true,
		KeyLetterSize:      18,
		KeyLetterBold:      false,
	}
	s.SystemDark = isSystemDarkMode()
	s.DarkMode = resolveEffectiveDark(s)
	return s
}

func resolveEffectiveDark(s AppSettings) bool {
	if s.Skin == "deep_blue" {
		return true
	}
	if s.ThemeMode == "light" {
		return false
	}
	if s.ThemeMode == "dark" {
		return true
	}
	return s.SystemDark
}

func clampInt(v, lo, hi int) int {
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}

func clampFloat(v, lo, hi float64) float64 {
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}

func normalizeSettings(s AppSettings) AppSettings {
	switch s.Scheme {
	case "pinyin", "ninekey", "flypy":
	default:
		s.Scheme = "flypy"
	}
	switch s.Skin {
	case "warm_grid", "mint", "deep_blue", "native_light":
	default:
		s.Skin = "native_light"
	}
	switch s.ThemeMode {
	case "light", "dark", "system":
	default:
		s.ThemeMode = "system"
	}
	s.FirstCandidateSize = clampInt(s.FirstCandidateSize, 16, 30)
	s.KeyLetterSize = clampInt(s.KeyLetterSize, 14, 30)
	s.KeyOpacity = math.Round(clampFloat(s.KeyOpacity, 0.4, 1.0)*100.0) / 100.0
	s.SystemDark = isSystemDarkMode()
	s.DarkMode = resolveEffectiveDark(s)
	return s
}

func settingsDataDir() (string, error) {
	base := os.Getenv("LOCALAPPDATA")
	if base == "" {
		var err error
		base, err = os.UserConfigDir()
		if err != nil || base == "" {
			base = os.TempDir()
		}
	}
	dir := filepath.Join(base, "FangCunIME")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	return dir, nil
}

func loadSettingsFile() AppSettings {
	s := defaultSettings()
	dir, err := settingsDataDir()
	if err != nil {
		return s
	}
	f, err := os.Open(filepath.Join(dir, "settings.ini"))
	if err != nil {
		return s
	}
	defer f.Close()

	hasThemeMode := false
	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		line := strings.TrimRight(scanner.Text(), "\r\n")
		eq := strings.IndexByte(line, '=')
		if eq < 0 {
			continue
		}
		key := line[:eq]
		val := line[eq+1:]
		switch key {
		case "scheme":
			s.Scheme = val
		case "mixed_input":
			s.MixedInput = val != "0"
		case "chinese_punctuation":
			s.ChinesePunctuation = val != "0"
		case "skin":
			s.Skin = val
		case "theme_mode":
			hasThemeMode = true
			s.ThemeMode = val
		case "dark":
			s.DarkMode = val == "1"
			if !hasThemeMode {
				if s.DarkMode {
					s.ThemeMode = "dark"
				} else {
					s.ThemeMode = "light"
				}
			}
		case "first_candidate_size":
			if n, err := strconv.Atoi(val); err == nil {
				s.FirstCandidateSize = n
			}
		case "key_opacity":
			if v, err := strconv.ParseFloat(val, 64); err == nil {
				s.KeyOpacity = v
			}
		case "typing_glow":
			s.TypingGlow = val != "0"
		case "key_letter_size":
			if n, err := strconv.Atoi(val); err == nil {
				s.KeyLetterSize = n
			}
		case "key_letter_bold":
			s.KeyLetterBold = val == "1"
		}
	}
	return normalizeSettings(s)
}

func saveSettingsFile(s AppSettings) AppSettings {
	s = normalizeSettings(s)
	dir, err := settingsDataDir()
	if err != nil {
		return s
	}

	b := func(v bool) string {
		if v {
			return "1"
		}
		return "0"
	}

	content := fmt.Sprintf(
		"scheme=%s\nmixed_input=%s\nchinese_punctuation=%s\nskin=%s\ntheme_mode=%s\ndark=%s\nfirst_candidate_size=%d\nkey_opacity=%.2f\ntyping_glow=%s\nkey_letter_size=%d\nkey_letter_bold=%s\n",
		s.Scheme,
		b(s.MixedInput),
		b(s.ChinesePunctuation),
		s.Skin,
		s.ThemeMode,
		b(s.DarkMode),
		s.FirstCandidateSize,
		s.KeyOpacity,
		b(s.TypingGlow),
		s.KeyLetterSize,
		b(s.KeyLetterBold),
	)
	_ = os.WriteFile(filepath.Join(dir, "settings.ini"), []byte(content), 0o644)
	broadcastSettingsChanged()
	return s
}
