// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package fuzz

import (
	"crypto/rand"
	"crypto/rsa"
	"crypto/x509"
	"encoding/pem"
	"fmt"
	"io"
	"io/ioutil"
	"net"
	"os"
	"path"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/golang/glog"
	"github.com/kr/fs"
	"github.com/pkg/sftp"
	"golang.org/x/crypto/ssh"
)

// A Connector is used to communicate with an instance
type Connector interface {
	// Connect establishes all necessary connections to the instance. It does
	// not need to be explicitly called, because the other Connector methods will
	// automatically connect if necessary, but may be called during initializiation.
	// It the connector is already connected, an error will be returned.
	Connect() error

	// Close closes any open connections to the instance. It is the client's
	// responsibility to call Close() when cleaning up the Connector.
	Close()

	// Returns an InstanceCmd representing the command to be run on the instance. Only one
	// command should be active at a time.
	// TODO(fxbug.dev/47479): In some cases, we should be able to relax the above restriction
	Command(name string, args ...string) InstanceCmd

	// Copies targetSrc (may include globs) to hostDst, which is always assumed
	// to be a directory. Directories are copied recursively.
	Get(targetSrc, hostDst string) error

	// Copies hostSrc (may include globs) to targetDst, which is always assumed
	// to be a directory. Directories are copied recursively.
	Put(hostSrc, targetDst string) error

	// Retrieves a syslog from the instance, filtered to the given process ID
	GetSysLog(pid int) (string, error)
}

// An SSHConnector is a Connector that uses SSH/SFTP for transport
// Note: exported fields will be serialized to the handle
type SSHConnector struct {
	// Host can be any IP or hostname as accepted by net.Dial
	Host string
	Port int
	// Key is a path to the SSH private key that should be used for
	// authentication
	Key string

	client     *ssh.Client
	sftpClient *sftp.Client

	// Retry configuration; defaults only need to be overridden for testing
	reconnectInterval time.Duration
}

const sshReconnectCount = 6
const defaultSSHReconnectInterval = 15 * time.Second

func NewSSHConnector(host string, port int, key string) *SSHConnector {
	return &SSHConnector{Host: host, Port: port, Key: key,
		reconnectInterval: defaultSSHReconnectInterval}
}

// Connect to the remote server
func (c *SSHConnector) Connect() error {
	if c.client != nil {
		return fmt.Errorf("Connect called, but already connected")
	}

	glog.Info("SSH: connecting...")
	key, err := ioutil.ReadFile(c.Key)
	if err != nil {
		return fmt.Errorf("error reading ssh key: %s", err)
	}

	signer, err := ssh.ParsePrivateKey(key)
	if err != nil {
		return fmt.Errorf("error parsing ssh key: %s", err)
	}

	config := &ssh.ClientConfig{
		User: "clusterfuchsia",
		Auth: []ssh.AuthMethod{
			ssh.PublicKeys(signer),
		},
		HostKeyCallback: ssh.InsecureIgnoreHostKey(),
	}

	address := net.JoinHostPort(c.Host, strconv.Itoa(c.Port))

	var client *ssh.Client
	first := true
	for j := 1; j <= sshReconnectCount; j++ {
		if !first {
			glog.Warningf("Retrying in %s...", c.reconnectInterval)
			time.Sleep(c.reconnectInterval)
		}
		first = false

		// TODO(fxbug.dev/45424): dial timeout
		if client, err = ssh.Dial("tcp", address, config); err != nil {
			glog.Warningf("Got error during attempt %d: %s", j, err)
			continue
		}

		glog.Info("SSH: connected")
		c.client = client

		sftpClient, err := sftp.NewClient(c.client)
		if err != nil {
			return fmt.Errorf("error connecting sftp: %s", err)
		}

		glog.Info("SFTP: connected")
		c.sftpClient = sftpClient

		return nil
	}

	return fmt.Errorf("error connecting ssh")

}

// Close any open connections
func (c *SSHConnector) Close() {
	glog.Info("Closing SSH/SFTP")

	// TODO(fxbug.dev/47316): Look into errors thrown by these Closes when
	// disconnecting from in-memory SSH server
	if c.client != nil {
		if err := c.client.Close(); err != nil {
			glog.Warningf("Error while closing SSH: %s", err)
		}
		c.client = nil
	}

	if c.sftpClient != nil {
		if err := c.sftpClient.Close(); err != nil {
			glog.Warningf("Error while closing SFTP: %s", err)
		}
		c.sftpClient = nil
	}
}

// Command returns an InstanceCmd that can be used to given command over SSH
func (c *SSHConnector) Command(name string, args ...string) InstanceCmd {
	// TODO(fxbug.dev/45424): Would be best to shell escape
	cmdline := strings.Join(append([]string{name}, args...), " ")
	return &SSHInstanceCmd{connector: c, cmdline: cmdline}
}

// GetSysLog will fetch the syslog by running a remote command
func (c *SSHConnector) GetSysLog(pid int) (string, error) {
	cmd := c.Command("log_listener", "--dump_logs", "--pid", strconv.Itoa(pid))

	out, err := cmd.Output()
	if err != nil {
		return "", err
	}
	return string(out), nil
}

// Get fetches files over SFTP
func (c *SSHConnector) Get(targetSrc string, hostDst string) error {
	if c.sftpClient == nil {
		if err := c.Connect(); err != nil {
			return err
		}
	}

	// Expand any globs in source path
	srcList, err := c.sftpClient.Glob(targetSrc)
	if err != nil {
		return fmt.Errorf("error during glob expansion: %s", err)
	}
	if len(srcList) == 0 {
		return fmt.Errorf("no files matching glob: '%s'", targetSrc)
	}

	for _, root := range srcList {
		walker := c.sftpClient.Walk(root)
		for walker.Step() {
			if err := walker.Err(); err != nil {
				return fmt.Errorf("error while walking %q: %s", root, err)
			}

			src := walker.Path()
			relPath, err := filepath.Rel(filepath.Dir(root), src)
			if err != nil {
				return fmt.Errorf("error taking relpath for %q: %s", src, err)
			}
			dst := path.Join(hostDst, relPath)

			// Create local directory if necessary
			if walker.Stat().IsDir() {
				if _, err := os.Stat(dst); os.IsNotExist(err) {
					os.Mkdir(dst, os.ModeDir|0755)
				}
				continue
			}

			glog.Infof("Copying [remote]:%s to %s", src, dst)

			fin, err := c.sftpClient.Open(src)
			if err != nil {
				return fmt.Errorf("error opening remote file: %s", err)
			}

			fout, err := os.Create(dst)
			if err != nil {
				fin.Close()
				return fmt.Errorf("error creating local file: %s", err)
			}

			_, err = io.Copy(fout, fin)

			// Close() immediately to free up resources since we're in a
			// potentially very large loop.
			fout.Close()
			fin.Close()

			if err != nil {
				return fmt.Errorf("error copying file: %s", err)
			}
		}
	}
	return nil
}

// Put uploads files over SFTP
func (c *SSHConnector) Put(hostSrc string, targetDst string) error {
	if c.sftpClient == nil {
		if err := c.Connect(); err != nil {
			return err
		}
	}

	// Expand any globs in source path
	srcList, err := filepath.Glob(hostSrc)
	if err != nil {
		return fmt.Errorf("error during glob expansion: %s", err)
	}
	if len(srcList) == 0 {
		return fmt.Errorf("no files matching glob: '%s'", hostSrc)
	}

	for _, root := range srcList {
		walker := fs.Walk(root)
		for walker.Step() {
			if err := walker.Err(); err != nil {
				return fmt.Errorf("error while walking %q: %s", root, err)
			}

			src := walker.Path()
			relPath, err := filepath.Rel(filepath.Dir(root), src)
			if err != nil {
				return fmt.Errorf("error taking relpath for %q: %s", src, err)
			}
			// filepath.Rel converts to host OS separators, while remote is always /
			dst := path.Join(targetDst, filepath.ToSlash(relPath))

			// Create remote subdirectory if necessary
			if walker.Stat().IsDir() {
				if _, err := c.sftpClient.Stat(dst); err == nil {
					continue
				} else if !os.IsNotExist(err) {
					return fmt.Errorf("error stat-ing remote directory %q: %s", dst, err)
				}

				if err := c.sftpClient.MkdirAll(dst); err != nil {
					return fmt.Errorf("error creating remote directory %q: %s", dst, err)
				}
				continue
			}

			// Create containing directories for remote file if necessary
			if err := c.sftpClient.MkdirAll(path.Dir(dst)); err != nil {
				return fmt.Errorf("error creating remote directory %q: %s", dst, err)
			}

			glog.Infof("Copying %s to [remote]:%s", src, dst)

			fin, err := os.Open(src)
			if err != nil {
				return fmt.Errorf("error opening local file: %s", err)
			}

			fout, err := c.sftpClient.Create(dst)
			if err != nil {
				fin.Close()
				return fmt.Errorf("error creating remote file: %s", err)
			}

			_, err = io.Copy(fout, fin)

			// Close() immediately to free up resources since we're in a
			// potentially very large loop.
			fout.Close()
			fin.Close()

			if err != nil {
				return fmt.Errorf("error copying file: %s", err)
			}
		}

	}
	return nil
}

func loadConnectorFromHandle(handle Handle) (Connector, error) {
	handleData, err := handle.GetData()
	if err != nil {
		return nil, err
	}

	// Check that the Connector is in a valid state
	switch conn := handleData.connector.(type) {
	case *SSHConnector:
		if conn.Host == "" {
			return nil, fmt.Errorf("host not found in handle")
		}
		if conn.Port == 0 {
			return nil, fmt.Errorf("port not found in handle")
		}
		if conn.Key == "" {
			return nil, fmt.Errorf("key not found in handle")
		}
		return conn, nil
	default:
		return nil, fmt.Errorf("unknown connector type: %T", handleData.connector)
	}
}

// Generate a key to use for SSH
func createSSHKey() (*rsa.PrivateKey, error) {
	privKey, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		return nil, fmt.Errorf("error generating keypair: %s", err)
	}

	return privKey, nil
}

// Writes private key to given path in format usable by SSH
func writeSSHPrivateKeyFile(key *rsa.PrivateKey, path string) error {
	pemData := pem.EncodeToMemory(&pem.Block{
		Type:  "RSA PRIVATE KEY",
		Bytes: x509.MarshalPKCS1PrivateKey(key),
	})

	if err := ioutil.WriteFile(path, pemData, 0o600); err != nil {
		return fmt.Errorf("error writing private key file: %s", err)
	}

	return nil
}

// Writes public key to given path in format usable by SSH
func writeSSHPublicKeyFile(key *rsa.PrivateKey, path string) error {
	pubKey, err := ssh.NewPublicKey(key.Public())
	if err != nil {
		return fmt.Errorf("error generating public key: %s", err)
	}
	pubKeyData := ssh.MarshalAuthorizedKey(pubKey)
	if err := ioutil.WriteFile(path, pubKeyData, 0o644); err != nil {
		return fmt.Errorf("error writing public key: %s", err)
	}

	return nil
}
