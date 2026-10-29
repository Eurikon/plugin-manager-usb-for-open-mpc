PYTHON ?= python3
ARM_IMAGE ?= mpc-usb-plugin-installer-build-tools
ARM64_IMAGE ?= mpc-usb-plugin-installer-build-tools:aarch64

OFFLINE_ARGS ?=
UPDATE_ARCH ?= aarch64
UPDATE_OUTPUT ?= $(CURDIR)/build/update-core-$(UPDATE_ARCH)-$(shell date +%d-%m-%Y-%H%M%S)
UPDATE_CORE_ARGS ?=
.DEFAULT_GOAL := arm
-include private/offline-tests.mk

.PHONY: arm64 arm64-image arm arm-helper arm-image test test-arm64 test-local test-runner test-offline test-update-core syntax site

arm: arm-image
	docker run --rm --platform linux/arm/v7 --network none -v "$(CURDIR):/work" -w /work $(ARM_IMAGE) sh build.sh

arm-image:
	docker build --platform linux/arm/v7 -t $(ARM_IMAGE) .

arm64: arm64-image
	docker run --rm --platform linux/arm64 --network none -v "$(CURDIR):/work" -w /work $(ARM64_IMAGE) env BUILD_DIR=build/aarch64 sh build.sh

arm64-image:
	docker build --platform linux/arm64 --build-arg BUILD_BASE=gcc:12 -t $(ARM64_IMAGE) .

arm-helper:
	docker run --rm --pull never --platform linux/arm/v7 --network none -v "$(CURDIR):/work" -w /work $(ARM_IMAGE) sh build_helper.sh

test: arm
	docker run --rm --pull never --platform linux/arm/v7 --network none -v "$(CURDIR):/work" -w /work $(ARM_IMAGE) build/plugin-storage-test
	docker run --rm --pull never --platform linux/arm/v7 --network none -v "$(CURDIR):/work" -w /work $(ARM_IMAGE) build/shortcut-environment-test
	docker run --rm --platform linux/arm/v7 --network none -v "$(CURDIR):/work" -w /work $(ARM_IMAGE) sh -c 'build/shortcut-state-test && MPC_PANEL_PORT=hw:1,0,1 MPC_SHORTCUT_SOCKET=/tmp/manager-hook-test.sock LD_LIBRARY_PATH=/work/build LD_PRELOAD=/work/build/shortcut-hook.so build/shortcut-hook-test'
	docker run --rm --platform linux/arm/v7 --network none -v "$(CURDIR):/work" -w /work $(ARM_IMAGE) sh -c 'MPC_PANEL_PORT=hw:1,0,1 MPC_SHORTCUT_SOCKET=/tmp/manager-hook-test.sock LD_LIBRARY_PATH=/work/build LD_PRELOAD="/work/build/shortcut-addin-first.so /work/build/shortcut-addin-second.so /work/build/shortcut-hook.so" build/shortcut-hook-test --chained'
	docker run --rm --pull never --platform linux/arm/v7 --network none -v "$(CURDIR):/work" -w /work $(ARM_IMAGE) sh -ec 'MPC_PANEL_PORT=hw:1,0,1 MPC_SHORTCUT_SOCKET=/tmp/manager-hook-test.sock LD_LIBRARY_PATH=/work/build LD_PRELOAD="/work/build/shortcut-lookup.so /work/build/shortcut-hook.so" build/shortcut-hook-test'

test-arm64: arm64
	docker run --rm --pull never --platform linux/arm64 --network none -v "$(CURDIR):/work" -w /work $(ARM64_IMAGE) build/aarch64/shortcut-environment-test
	docker run --rm --pull never --platform linux/arm64 --network none -v "$(CURDIR):/work" -w /work $(ARM64_IMAGE) sh -ec 'for check in plugin-storage-test architecture-test sd-install-test manager-update-test shortcut-state-test; do build/aarch64/$$check; done; MPC_PANEL_PORT=hw:1,0,1 MPC_SHORTCUT_SOCKET=/tmp/manager-hook-test.sock LD_LIBRARY_PATH=/work/build/aarch64 LD_PRELOAD=/work/build/aarch64/shortcut-hook.so build/aarch64/shortcut-hook-test; MPC_PANEL_PORT=hw:1,0,1 MPC_SHORTCUT_SOCKET=/tmp/manager-hook-test.sock LD_LIBRARY_PATH=/work/build/aarch64 LD_PRELOAD="/work/build/aarch64/shortcut-addin-first.so /work/build/aarch64/shortcut-addin-second.so /work/build/aarch64/shortcut-hook.so" build/aarch64/shortcut-hook-test --chained'
	docker run --rm --pull never --platform linux/arm64 --network none -v "$(CURDIR):/work" -w /work $(ARM64_IMAGE) sh -ec 'MPC_PANEL_PORT=hw:1,0,1 MPC_SHORTCUT_SOCKET=/tmp/manager-hook-test.sock LD_LIBRARY_PATH=/work/build/aarch64 LD_PRELOAD="/work/build/aarch64/shortcut-lookup.so /work/build/aarch64/shortcut-hook.so" build/aarch64/shortcut-hook-test'

test-runner:
	TEST_PACKAGE_ZIP="$(TEST_PACKAGE_ZIP)" LEGACY_PACKAGE_ZIP="$(LEGACY_PACKAGE_ZIP)" $(PYTHON) tests/offline_runner_test.py

test-update-core:
	$(PYTHON) tests/run_update_core.py --arch "$(UPDATE_ARCH)" --output "$(UPDATE_OUTPUT)" $(UPDATE_CORE_ARGS)

test-local:
	TEST_PACKAGE_ZIP="$(TEST_PACKAGE_ZIP)" LEGACY_PACKAGE_ZIP="$(LEGACY_PACKAGE_ZIP)" $(PYTHON) -m unittest discover -s tests -p offline_runner_test.py
	TEST_PACKAGE_ZIP="$(TEST_PACKAGE_ZIP)" $(PYTHON) -m unittest discover -s tests -p package_test.py
	$(PYTHON) -m unittest discover -s tests -p firmware_test.py
	$(PYTHON) -m unittest discover -s tests -p saltscan_test.py
	$(PYTHON) -m unittest discover -s tests -p setup_test.py
	$(PYTHON) -m unittest discover -s tests -p launch_test.py
	$(PYTHON) -m unittest discover -s tests -p bootstrap_running_mpc_test.py
	$(PYTHON) -m unittest discover -s tests -p startup_package_test.py
	$(PYTHON) -m unittest discover -s tests -p bootstrap_failure_note_test.py

site:
	$(PYTHON) tools/build_site.py

test-offline:
	@test -n "$(FIRMWARE_ROOT)" -a -n "$(TEST_PACKAGE_ZIP)" || { echo 'Set FIRMWARE_ROOT and TEST_PACKAGE_ZIP'; exit 1; }
	$(PYTHON) tests/run_offline.py "$(FIRMWARE_ROOT)" "$(TEST_PACKAGE_ZIP)" $(if $(LEGACY_PACKAGE_ZIP),--legacy-package "$(LEGACY_PACKAGE_ZIP)" $(if $(filter 1 yes true,$(LEGACY_OS3_CONFIRMED)),--legacy-os3-confirmed)) --image "$(ARM_IMAGE)" $(OFFLINE_ARGS)

syntax:
	$(PYTHON) -m py_compile package.py tools/*.py tests/*.py
	@for script in *.sh src/startup/*.sh; do sh -n "$$script" || exit; done
