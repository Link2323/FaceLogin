package internal

import (
	"encoding/json"
	"fmt"
	"os"
)

const (
	defaultMatchThreshold     = 0.80
	defaultAntiSpoofThreshold = 0.28
)

// EnsureConfigDefaults creates the current config.json for a fresh install.
// Existing configurations are unsupported by this release and are left intact
// so a failed or incomplete uninstall is never silently rewritten.
func EnsureConfigDefaults(configPath string) error {
	if _, err := os.Stat(configPath); err == nil {
		return nil
	} else if !os.IsNotExist(err) {
		return fmt.Errorf("stat config %s: %w", configPath, err)
	}

	cfg := map[string]any{
		"match_threshold":           defaultMatchThreshold,
		"anti_spoof_threshold":      defaultAntiSpoofThreshold,
		"low_light_enhance":         true,
		"camera_rotation":           0,
		"camera_device":             "",
		"ema_learning":              true,
		"failure_learning":          true,
		"learning_alpha":            0.10,
		"learning_distance_gate":    0.65,
		"learning_norm_floor":       19.1,
		"learning_min_interval_sec": 60,
	}
	out, err := json.MarshalIndent(cfg, "", "  ")
	if err != nil {
		return fmt.Errorf("marshal config: %w", err)
	}
	if err := os.WriteFile(configPath, out, 0644); err != nil {
		return fmt.Errorf("write config %s: %w", configPath, err)
	}
	return nil
}
