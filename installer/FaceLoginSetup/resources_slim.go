//go:build slim

package main

import "embed"

// resources is a zero-value embed.FS in the slim uninstaller build: reading
// "resources" from it always fails, which is exactly what the uninstall
// flow's fallback path expects — RemoveInstalledFiles then deletes from the
// static currentRootPayloadFiles manifest. The slim build must never embed
// the payload (that is the whole point: ~9 MB instead of ~95 MB).
var resources embed.FS

const uninstallerBuild = true
