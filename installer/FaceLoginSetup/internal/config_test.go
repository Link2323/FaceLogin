package internal

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func TestEnsureConfigDefaultsUsesProductionAuthenticationDefaults(t *testing.T) {
	configPath := filepath.Join(t.TempDir(), "config.json")
	if err := EnsureConfigDefaults(configPath); err != nil {
		t.Fatalf("EnsureConfigDefaults returned error: %v", err)
	}

	data, err := os.ReadFile(configPath)
	if err != nil {
		t.Fatalf("read generated config: %v", err)
	}
	var cfg map[string]any
	if err := json.Unmarshal(data, &cfg); err != nil {
		t.Fatalf("generated config is invalid JSON: %v", err)
	}

	if got := cfg["match_threshold"]; got != defaultMatchThreshold {
		t.Fatalf("fresh-install match_threshold = %v, want %.2f", got, defaultMatchThreshold)
	}
	if got := cfg["anti_spoof_threshold"]; got != defaultAntiSpoofThreshold {
		t.Fatalf("fresh-install anti_spoof_threshold = %v, want %.3f", got, defaultAntiSpoofThreshold)
	}
	if got := cfg["liveness_method"]; got != "antispoof" {
		t.Fatalf("fresh-install liveness_method = %v, want antispoof", got)
	}
}
