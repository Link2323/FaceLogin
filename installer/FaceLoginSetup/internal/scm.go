package internal

import (
	"fmt"
	"syscall"
	"time"

	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/svc/mgr"
)

const SERVICE_NAME = "FaceLoginService"

// ServiceExists checks if the named service is installed.
func ServiceExists(name string) (bool, error) {
	m, err := mgr.Connect()
	if err != nil {
		return false, fmt.Errorf("connect SCM: %w", err)
	}
	defer m.Disconnect()

	s, err := m.OpenService(name)
	if err != nil {
		if err == windows.ERROR_SERVICE_DOES_NOT_EXIST {
			return false, nil
		}
		// A service marked for deletion is still present until all handles are
		// closed and the SCM completes the asynchronous removal.
		if err == windows.ERROR_SERVICE_MARKED_FOR_DELETE {
			return true, nil
		}
		return false, fmt.Errorf("open service %q: %w", name, err)
	}
	s.Close()
	return true, nil
}

// stopService kills the service process and waits for SCM to observe STOPPED.
// FaceLoginService does not handle a graceful SCM stop request, so the
// process is terminated directly. The wait closes the race between process
// exit and the SCM state update.
func stopService(s *mgr.Service) error {
	if s == nil {
		return nil
	}

	_, _ = RunCommand("taskkill", "/f", "/im", "FaceLoginService.exe")

	for i := 0; i < 10; i++ {
		time.Sleep(500 * time.Millisecond)
		status, err := s.Query()
		if err != nil {
			if err == windows.ERROR_SERVICE_DOES_NOT_EXIST ||
				err == windows.ERROR_SERVICE_MARKED_FOR_DELETE {
				return nil
			}
			return fmt.Errorf("query service status: %w", err)
		}
		if status.State == windows.SERVICE_STOPPED {
			return nil
		}
	}

	// The process was killed; SCM can still be slow to publish STOPPED. The
	// caller may proceed to Delete and handle the marked-for-delete result.
	return nil
}

// waitForServiceGone waits for SCM to finish an asynchronous DeleteService.
func waitForServiceGone(name string) error {
	for i := 0; i < 20; i++ {
		exists, err := ServiceExists(name)
		if err != nil {
			return err
		}
		if !exists {
			return nil
		}
		time.Sleep(500 * time.Millisecond)
	}
	return fmt.Errorf("service %q is still present after waiting for deletion", name)
}

// StopAndDeleteService stops (if running) and deletes the service.
func StopAndDeleteService() error {
	exists, err := ServiceExists(SERVICE_NAME)
	if err != nil {
		return err
	}
	if !exists {
		return nil
	}

	m, err := mgr.Connect()
	if err != nil {
		return fmt.Errorf("connect SCM: %w", err)
	}
	defer m.Disconnect()

	// Open the service once, stop it, then delete it. Opening first lets us
	// query SCM state instead of guessing with a fixed sleep.
	s, err := m.OpenService(SERVICE_NAME)
	if err != nil {
		if err == windows.ERROR_SERVICE_DOES_NOT_EXIST {
			return nil
		}
		if err == windows.ERROR_SERVICE_MARKED_FOR_DELETE {
			return waitForServiceGone(SERVICE_NAME)
		}
		return fmt.Errorf("open for delete: %w", err)
	}

	stopErr := stopService(s)
	err = s.Delete()
	s.Close()
	if err != nil {
		if err == windows.ERROR_SERVICE_MARKED_FOR_DELETE {
			if waitErr := waitForServiceGone(SERVICE_NAME); waitErr != nil {
				return fmt.Errorf("service marked for deletion but did not disappear: %w", waitErr)
			}
			return nil
		}
		if stopErr != nil {
			return fmt.Errorf("delete service after stop failed: %v: %w", stopErr, err)
		}
		return fmt.Errorf("delete service: %w", err)
	}

	return waitForServiceGone(SERVICE_NAME)
}

func createService(m *mgr.Mgr, exePath string) (*mgr.Service, error) {
	return m.CreateService(
		SERVICE_NAME,
		exePath,
		mgr.Config{
			StartType:    mgr.StartAutomatic,
			ErrorControl: mgr.ErrorNormal,
			DisplayName:  "FaceLogin 人脸认证服务",
			Description:  "FaceLogin — 基于人脸识别的 Windows 登录认证服务",
			ServiceType:  windows.SERVICE_WIN32_OWN_PROCESS,
		},
	)
}

// InstallService creates (or updates) and starts the service.
func InstallService(exePath string) error {
	m, err := mgr.Connect()
	if err != nil {
		return fmt.Errorf("connect SCM: %w", err)
	}
	defer m.Disconnect()

	// Stop an existing service before replacing its executable. Keep the SCM
	// handle open while querying status, then close it before CreateService.
	if existing, openErr := m.OpenService(SERVICE_NAME); openErr == nil {
		if err := stopService(existing); err != nil {
			existing.Close()
			return fmt.Errorf("stop existing service: %w", err)
		}
		existing.Close()
	} else if openErr != windows.ERROR_SERVICE_DOES_NOT_EXIST {
		if openErr == windows.ERROR_SERVICE_MARKED_FOR_DELETE {
			if err := waitForServiceGone(SERVICE_NAME); err != nil {
				return err
			}
		} else {
			return fmt.Errorf("open existing service: %w", openErr)
		}
	}

	// Try to create. A service that disappeared asynchronously may still
	// return 1072 once; wait for the name to become reusable and retry once.
	s, err := createService(m, exePath)
	if err == windows.ERROR_SERVICE_MARKED_FOR_DELETE {
		if waitErr := waitForServiceGone(SERVICE_NAME); waitErr != nil {
			return fmt.Errorf("wait for marked-for-delete service: %w", waitErr)
		}
		s, err = createService(m, exePath)
	}
	if err == windows.ERROR_SERVICE_EXISTS {
		// Service already exists — open and update config.
		s, err = m.OpenService(SERVICE_NAME)
		if err != nil {
			return fmt.Errorf("open existing service: %w", err)
		}
		cfg, cfgErr := s.Config()
		if cfgErr != nil {
			s.Close()
			return fmt.Errorf("get service config: %w", cfgErr)
		}
		// EscapeArg is required on the update path for install directories with
		// spaces; preserve this invariant when updating an existing service.
		cfg.BinaryPathName = syscall.EscapeArg(exePath)
		cfg.StartType = mgr.StartAutomatic
		if cfgErr = s.UpdateConfig(cfg); cfgErr != nil {
			s.Close()
			return fmt.Errorf("update service config: %w", cfgErr)
		}
	}
	if err != nil {
		return fmt.Errorf("create service: %w", err)
	}
	defer s.Close()

	// Start the service
	err = s.Start()
	if err != nil && err != windows.ERROR_SERVICE_ALREADY_RUNNING {
		return fmt.Errorf("start service: %w", err)
	}
	return nil
}
