// Copyright (c) 2026 Jonathan Elliot Peace <jep@alphabetiq.com>
// SPDX-License-Identifier: Apache-2.0
//
// PiZZa Phase-6 host uploader. Pushes a sketch .llext/.elf to the PiZZa loader
// over USB CDC, in-process and with no reboot:
//
//  1. 1200-bps touch: open the port at 1200 baud, hold ~0.6 s so the loader's
//     baud poll sees it, then close. The loader aborts the running sketch and
//     switches to upload-listen.
//  2. Reopen at the data baud and send: "PZUP" + <le32 length> + <bytes>. The
//     loader writes it to /SD:/sketch.llext and replies "OK\n".
//
// Pure Go (go.bug.st/serial -- the library arduino-cli itself uses), so it
// cross-compiles to a static, dependency-free binary for every host with no
// cgo. Usage: cdc-upload <serial-port> <sketch-file>
package main

import (
	"encoding/binary"
	"fmt"
	"os"
	"time"

	"go.bug.st/serial"
)

const (
	touchBaud   = 1200
	dataBaud    = 115200
	touchHold   = 600 * time.Millisecond
	touchSettle = 400 * time.Millisecond
	replyWait   = 8 * time.Second
)

func fail(format string, a ...interface{}) {
	fmt.Fprintf(os.Stderr, "cdc-upload: "+format+"\n", a...)
	os.Exit(1)
}

func main() {
	if len(os.Args) != 3 {
		fail("usage: cdc-upload <serial-port> <sketch-file>")
	}
	portName, path := os.Args[1], os.Args[2]

	data, err := os.ReadFile(path)
	if err != nil {
		fail("read %s: %v", path, err)
	}

	// 1) 1200-bps touch.
	fmt.Printf("cdc-upload: 1200-bps touch on %s...\n", portName)
	t, err := serial.Open(portName, &serial.Mode{BaudRate: touchBaud})
	if err != nil {
		fail("touch open failed: %v", err)
	}
	time.Sleep(touchHold)
	t.Close()
	time.Sleep(touchSettle) // let the loader enter upload-listen

	// 2) Reopen and send the framed sketch.
	port, err := serial.Open(portName, &serial.Mode{BaudRate: dataBaud})
	if err != nil {
		fail("reopen failed: %v", err)
	}
	defer port.Close()
	time.Sleep(200 * time.Millisecond)
	port.ResetInputBuffer()

	hdr := make([]byte, 8)
	copy(hdr, "PZUP")
	binary.LittleEndian.PutUint32(hdr[4:], uint32(len(data)))
	if _, err := port.Write(hdr); err != nil {
		fail("write header: %v", err)
	}
	if _, err := port.Write(data); err != nil {
		fail("write data: %v", err)
	}
	port.Drain()
	fmt.Printf("cdc-upload: sent %d bytes; waiting for device...\n", len(data))

	port.SetReadTimeout(replyWait)
	line := readLine(port)
	if line == "" {
		line = "(no response)"
	}
	fmt.Printf("cdc-upload: device: %s\n", line)
	if line != "OK" {
		os.Exit(1)
	}
}

// readLine reads one '\n'-terminated line (or until the read times out).
func readLine(port serial.Port) string {
	var buf []byte
	b := make([]byte, 1)
	for {
		n, err := port.Read(b)
		if err != nil || n == 0 {
			break // error or read timeout
		}
		if b[0] == '\n' {
			break
		}
		if b[0] != '\r' {
			buf = append(buf, b[0])
		}
	}
	return string(buf)
}
