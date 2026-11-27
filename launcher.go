package main

import (
	_ "embed"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
)

//go:embed index.html
var game []byte

func launch() error {
	root, err := os.UserConfigDir()
	if err != nil {
		return err
	}
	dir := filepath.Join(root, "FastSim")
	if err := os.MkdirAll(dir, 0700); err != nil {
		return err
	}
	page := filepath.Join(dir, "index.html")
	if err := os.WriteFile(page, game, 0600); err != nil {
		return err
	}
	if runtime.GOOS == "windows" {
		return exec.Command("rundll32.exe", "url.dll,FileProtocolHandler", page).Run()
	}
	return exec.Command("xdg-open", page).Run()
}

func main() {
	if err := launch(); err != nil {
		fmt.Fprintln(os.Stderr, "Fast Sim:", err)
		os.Exit(1)
	}
}
