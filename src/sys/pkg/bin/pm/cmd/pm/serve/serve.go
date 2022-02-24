// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package serve

import (
	"compress/gzip"
	"flag"
	"fmt"
	"io/ioutil"
	"log"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"go.fuchsia.dev/fuchsia/src/sys/pkg/bin/pm/build"
	"go.fuchsia.dev/fuchsia/src/sys/pkg/bin/pm/fswatch"
	"go.fuchsia.dev/fuchsia/src/sys/pkg/bin/pm/pmhttp"
	"go.fuchsia.dev/fuchsia/src/sys/pkg/bin/pm/repo"
)

// server is a default http server only parameterized for tests.
var server http.Server

var (
	fs            = flag.NewFlagSet("serve", flag.ExitOnError)
	repoServeDir  = fs.String("d", "", "(deprecated, use -repo) path to the repository")
	listen        = fs.String("l", ":8083", "HTTP listen address")
	auto          = fs.Bool("a", true, "Host auto endpoint for realtime client updates")
	quiet         = fs.Bool("q", false, "Don't print out information about requests")
	encryptionKey = fs.String("e", "", "Path to a symmetric blob encryption key *UNSAFE*")
	publishList   = fs.String("p", "", "path to a package list file to be auto-published")
	portFile      = fs.String("f", "", "path to a file to write the HTTP listen port")
	configVersion = fs.Int("c", 1, "component framework version for config.json")
	persist       = fs.Bool("persist", false, "request clients to persist TUF metadata for this repository (supported only with `-c 2`)")
	config        = &repo.Config{}
	initOnce      sync.Once
)

func ParseFlags(args []string) error {
	// the flags added by vars can't be added more than once, so when tests invoke
	// this func more than once, it causes a failure.
	initOnce.Do(func() { config.Vars(fs) })

	fs.Usage = func() {
		fmt.Fprintf(os.Stderr, "usage: %s serve", filepath.Base(os.Args[0]))
		fmt.Fprintln(os.Stderr)
		fs.PrintDefaults()
	}

	if err := fs.Parse(args); err != nil {
		return err
	}
	if len(fs.Args()) != 0 {
		fmt.Fprintf(os.Stderr, "WARNING: unused arguments: %s\n", fs.Args())
	}
	config.ApplyDefaults()

	// The -d flag points at $reporoot/repository, so the "repo" for publishing is
	// one directory above that.
	// If -d is passed, it takes priority.
	if *repoServeDir == "" {
		*repoServeDir = filepath.Join(config.RepoDir, "repository")
	} else {
		config.RepoDir = filepath.Dir(*repoServeDir)
	}
	return nil
}

func Run(cfg *build.Config, args []string, addrChan chan string) error {
	if err := ParseFlags(args); err != nil {
		return err
	}

	repo, err := repo.New(config.RepoDir, filepath.Join(config.RepoDir, "repository", "blobs"))
	if err != nil {
		return err
	}
	if *encryptionKey != "" {
		repo.EncryptWith(*encryptionKey)
	}

	if err := repo.Init(); err != nil && err != os.ErrExist {
		return fmt.Errorf("repository at %q is not valid or could not be initialized: %s", config.RepoDir, err)
	}

	mux := http.NewServeMux()

	if *auto {
		as := pmhttp.NewAutoServer()

		// This needs to be the first defer to guarantee the channels ranged over in the
		// waited-upon goroutines are closed by subsequent defer cleanup.
		var wg sync.WaitGroup
		defer wg.Wait()

		w, err := fswatch.NewWatcher()
		if err != nil {
			return fmt.Errorf("failed to initialize fsnotify: %s", err)
		}
		defer w.Close()

		timestampPath := filepath.Join(*repoServeDir, "timestamp.json")
		timestampMonitor := NewMetadataMonitor(timestampPath, w)
		defer timestampMonitor.Close()

		wg.Add(3)
		go func() {
			defer wg.Done()
			for metadata := range timestampMonitor.Events {
				if !*quiet {
					log.Printf("[pm auto] notify new timestamp.json version: %v", metadata.Version)
				}
				as.Broadcast("timestamp.json", fmt.Sprintf("%v", metadata.Version))
			}
		}()

		go func() {
			defer wg.Done()
			// Drain any watch errors encountered, or the first error will block the filesystem watcher
			// forever.
			for err := range w.Errors {
				log.Printf("[pm auto] watch error: %s. Events/monitors may be lost.", err)
			}
		}()

		go func() {
			defer wg.Done()
			for event := range w.Events {
				switch event.Name {
				case timestampPath:
					timestampMonitor.HandleEvent(event)
				}
			}
		}()

		mux.Handle("/auto", as)

		if *publishList != "" {
			mw, err := NewManifestWatcher(publishList, quiet)
			if err != nil {
				return fmt.Errorf("[pm auto] unable to create incremental manifest watcher: %s", err)
			}
			defer mw.stop()
			wg.Add(1)
			go func() {
				defer wg.Done()
				for manifests := range mw.PublishEvents {
					_, err = repo.PublishManifests(manifests)
					if err != nil {
						log.Fatalf("[pm auto] unable to publish manifests %v: %s", manifests, err)
					}
					if err := repo.CommitUpdates(config.TimeVersioned); err != nil {
						log.Fatalf("[pm auto] committing repo: %s", err)
					}
				}
			}()
			if err := mw.start(); err != nil {
				return fmt.Errorf("[pm auto] failed to start incremental manifest watcher: %s", err)
			}
		}
	}

	dirServer := http.FileServer(http.Dir(*repoServeDir))
	mux.Handle("/", http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/":
			pmhttp.ServeIndex(w)
		case "/js":
			pmhttp.ServeJS(w)
		default:
			dirServer.ServeHTTP(w, r)
		}
	}))

	switch *configVersion {
	case 1:
		cs := pmhttp.NewConfigServer(func() []byte {
			b, err := ioutil.ReadFile(filepath.Join(*repoServeDir, "root.json"))
			if err != nil {
				log.Printf("%s", err)
			}
			return b
		}, *encryptionKey)
		mux.Handle("/config.json", cs)
	case 2:
		cs := pmhttp.NewConfigServerV2(func() []byte {
			b, err := ioutil.ReadFile(filepath.Join(*repoServeDir, "root.json"))
			if err != nil {
				log.Printf("%s", err)
			}
			return b
		}, *persist)
		mux.Handle("/config.json", cs)
	default:
		return fmt.Errorf("[pm auto] invalid component version specified: %v", *configVersion)
	}

	server.Handler = http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !strings.HasPrefix(r.RequestURI, "/blobs") && strings.Contains(r.Header.Get("Accept-Encoding"), "gzip") {
			gw := &pmhttp.GZIPWriter{
				w,
				gzip.NewWriter(w),
			}
			defer gw.Close()
			gw.Header().Set("Content-Encoding", "gzip")
			w = gw
		}
		lw := &pmhttp.LoggingWriter{w, 0, 0}
		mux.ServeHTTP(lw, r)
		if !*quiet {
			fmt.Printf("%s [pm serve] %s \"%s %s %s\" %d %d\n",
				time.Now().Format("2006-01-02 15:04:05"),
				r.RemoteAddr,
				r.Method,
				r.RequestURI,
				r.Proto,
				lw.Status,
				lw.ResponseSize)
		}
	})

	listener, err := net.Listen("tcp", *listen)
	if err != nil {
		return err
	}

	addr := listener.Addr().String()
	if addrChan != nil {
		addrChan <- addr
	}

	if *portFile != "" {
		_, port, err := net.SplitHostPort(addr)
		if err != nil {
			return fmt.Errorf("error splitting addr into host and port %s: %s", addr, err)
		}
		portFileTmp := fmt.Sprintf("%s.tmp", *portFile)
		if err := ioutil.WriteFile(portFileTmp, []byte(port), 0644); err != nil {
			return fmt.Errorf("error creating tmp port file %s: %s", portFileTmp, err)
		}
		if err := os.Rename(portFileTmp, *portFile); err != nil {
			return fmt.Errorf("error renaming port file from %s to %s:  %s", portFileTmp, *portFile, err)
		}
	}

	if !*quiet {
		fmt.Printf("%s [pm serve] serving %s at http://%s\n",
			time.Now().Format("2006-01-02 15:04:05"), config.RepoDir, addr)
	}

	return server.Serve(listener)
}
