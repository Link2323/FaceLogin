package internal

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
)

// The installer hard-codes values that also live in the C++ tree (CLSID,
// config defaults). These tests pin the copies together so a one-side edit
// fails the build instead of shipping silently inconsistent binaries.
//
// go test runs with the package directory (internal/) as the working
// directory, so the repository root is three levels up.

func repoRead(t *testing.T, rel ...string) string {
	t.Helper()
	p := filepath.Join(append([]string{"..", "..", ".."}, rel...)...)
	data, err := os.ReadFile(p)
	if err != nil {
		t.Fatalf("cannot read %s (%v) — run tests from the repository checkout", p, err)
	}
	return string(data)
}

// TestCredentialProviderCLSIDConsistent treats the DEFINE_GUID in
// credential_provider/resource.h as the source of truth and requires every
// string form of the CLSID — the resource.h comment, dllmain.cpp's
// CLSID_STRING, the registry path in FaceLoginProvider.cpp and this
// installer's cleanup constant — to encode the same value.
func TestCredentialProviderCLSIDConsistent(t *testing.T) {
	resourceH := repoRead(t, "credential_provider", "resource.h")

	defineRe := regexp.MustCompile(`(?s)DEFINE_GUID\s*\(\s*CLSID_\w+\s*,\s*` +
		`((?:0x[0-9A-Fa-f]+\s*,\s*){10}0x[0-9A-Fa-f]+)\s*\)`)
	m := defineRe.FindStringSubmatch(resourceH)
	if m == nil {
		t.Fatal("cannot parse DEFINE_GUID(CLSID_FaceLoginProvider, ...) in credential_provider/resource.h")
	}
	fields := regexp.MustCompile(`0x[0-9A-Fa-f]+`).FindAllString(m[1], -1)
	if len(fields) != 11 {
		t.Fatalf("DEFINE_GUID has %d hex fields, want 11", len(fields))
	}
	parse := func(s string, bits int) uint64 {
		v, err := strconv.ParseUint(strings.TrimPrefix(s, "0x"), 16, bits)
		if err != nil {
			t.Fatalf("parse DEFINE_GUID field %s: %v", s, err)
		}
		return v
	}
	canonical := fmt.Sprintf("{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
		parse(fields[0], 32), parse(fields[1], 16), parse(fields[2], 16),
		parse(fields[3], 8), parse(fields[4], 8), parse(fields[5], 8), parse(fields[6], 8),
		parse(fields[7], 8), parse(fields[8], 8), parse(fields[9], 8), parse(fields[10], 8))

	occurrences := []struct {
		name string
		path []string
		re   *regexp.Regexp
	}{
		{"resource.h comment", []string{"credential_provider", "resource.h"},
			regexp.MustCompile(`(\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\})`)},
		{"dllmain.cpp CLSID_STRING", []string{"credential_provider", "dllmain.cpp"},
			regexp.MustCompile(`CLSID_STRING[^\n]*?L"(\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\})"`)},
		{"FaceLoginProvider.cpp registry path", []string{"credential_provider", "FaceLoginProvider.cpp"},
			regexp.MustCompile(`Credential Providers\\\\(\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\})`)},
	}
	for _, occ := range occurrences {
		found := occ.re.FindAllStringSubmatch(repoRead(t, occ.path...), -1)
		if len(found) == 0 {
			t.Errorf("%s: no CLSID string found — copy may have been refactored; update this test", occ.name)
			continue
		}
		for _, f := range found {
			if !strings.EqualFold(f[1], canonical) {
				t.Errorf("%s: CLSID %s does not match DEFINE_GUID %s in resource.h", occ.name, f[1], canonical)
			}
		}
	}

	if !strings.EqualFold(clsidCredentialProvider, canonical) {
		t.Errorf("installer clsidCredentialProvider %s does not match DEFINE_GUID %s in resource.h",
			clsidCredentialProvider, canonical)
	}
}

// TestInstallerConfigDefaultsMatchCPlusPlus requires the config.json written
// by EnsureConfigDefaults to agree with the C++ reader: every written key must
// be one that ConfigFromJson reads, and every written value must equal the
// C++ default, so a fresh install behaves identically to a config with
// missing keys.
func TestInstallerConfigDefaultsMatchCPlusPlus(t *testing.T) {
	dir := t.TempDir()
	configPath := filepath.Join(dir, "config.json")
	if err := EnsureConfigDefaults(configPath); err != nil {
		t.Fatalf("EnsureConfigDefaults: %v", err)
	}
	raw, err := os.ReadFile(configPath)
	if err != nil {
		t.Fatalf("read written config: %v", err)
	}
	var written map[string]any
	if err := json.Unmarshal(raw, &written); err != nil {
		t.Fatalf("parse written config: %v", err)
	}

	configH := repoRead(t, "common", "config_util.h")
	configCpp := repoRead(t, "common", "config_util.cpp")
	profileH := repoRead(t, "common", "recognizer_profile.h")

	// Keys the C++ side actually reads (ConfigFromJson).
	cppKeys := map[string]bool{}
	for _, k := range regexp.MustCompile(`jsonGet(?:String|Float|Int)\(json, "(\w+)"`).
		FindAllStringSubmatch(configCpp, -1) {
		cppKeys[k[1]] = true
	}
	for key := range written {
		if !cppKeys[key] {
			t.Errorf("installer writes config key %q that ConfigFromJson never reads", key)
		}
	}

	// C++ struct defaults from AppConfig.
	cppDefaults := map[string]string{}
	for _, mm := range regexp.MustCompile(`(?m)^\s*(?:float|int|bool|std::string)\s+(\w+)\s*=\s*([^;]+);`).
		FindAllStringSubmatch(configH, -1) {
		cppDefaults[mm[1]] = strings.TrimSpace(mm[2])
	}
	thresholdM := regexp.MustCompile(`kDefaultMatchThreshold\s*=\s*([\d.]+)f`).FindStringSubmatch(profileH)
	if thresholdM == nil {
		t.Fatal("cannot parse kDefaultMatchThreshold in common/recognizer_profile.h")
	}

	// resolve maps a written config key to the value the C++ side defaults to.
	resolve := func(key string) (any, bool) {
		switch key {
		case "match_threshold":
			v, err := strconv.ParseFloat(thresholdM[1], 64)
			return v, err == nil
		case "recognizer_model":
			// Not an AppConfig field; ConfigFromJson only accepts one value.
			m := regexp.MustCompile(`jsonGetString\(json, "recognizer_model"\) == "([^"]+)"`).
				FindStringSubmatch(configCpp)
			if m == nil {
				return nil, false
			}
			return m[1], true
		}
		def, ok := cppDefaults[key]
		if !ok {
			return nil, false
		}
		switch {
		case def == "true" || def == "false":
			return def == "true", true
		case def == `""`:
			return "", true
		case strings.HasSuffix(def, "f"):
			v, err := strconv.ParseFloat(strings.TrimSuffix(def, "f"), 64)
			return v, err == nil
		default:
			v, err := strconv.ParseFloat(def, 64)
			return v, err == nil
		}
	}

	for key, got := range written {
		want, ok := resolve(key)
		if !ok {
			t.Errorf("config key %q: no C++ default found to compare against", key)
			continue
		}
		if wantFloat, isFloat := want.(float64); isFloat {
			gotFloat, isNumber := got.(float64)
			if !isNumber || math.Abs(gotFloat-wantFloat) > 1e-6 {
				t.Errorf("config %q = %v, C++ default is %v", key, got, want)
			}
			continue
		}
		if got != want {
			t.Errorf("config %q = %v, C++ default is %v", key, got, want)
		}
	}
}
