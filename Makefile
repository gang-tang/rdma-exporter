# Makefile for get_rdma_devices.cpp

# --- Build Variables ---
CXX      := g++
TARGET   := munic_exporter
SRC      := main.cpp rdma_qp_counters.cpp
LIBS     := -lpthread

# Build type: release (default) or debug
BUILD ?= release

ifeq ($(BUILD),debug)
    CXXFLAGS := -std=c++17 -Wall -Wextra -O0 -g
else
    CXXFLAGS := -std=c++17 -Wall -Wextra -O2
endif

# Handle older libstdc++ filesystem linking
LDFLAGS  := $(shell echo | $(CXX) -std=c++17 -x c++ - -o /dev/null 2>&1 | grep -q 'undefined reference' && echo '-lstdc++fs')

# --- Service/Install Variables ---
SERVICE_NAME := munic-exporter
SERVICE_USER := s1
INSTALL_DIR  := /opt/$(SERVICE_NAME)
EXECUTABLE   := $(INSTALL_DIR)/$(SERVICE_NAME)
SERVICE_FILE := /etc/systemd/system/$(SERVICE_NAME).service

# --- Systemd Service Content ---
# We use 'define' to keep the multi-line service file readable
define SERVICE_CONTENT
[Unit]
Description=$(SERVICE_NAME) service
After=network.target

[Service]
Type=simple
User=$(SERVICE_USER)
ExecStart=$(EXECUTABLE)
Restart=on-failure
RestartSec=5s

[Install]
WantedBy=multi-user.target
endef
export SERVICE_CONTENT

# --- Rules ---

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LIBS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	@echo "--- Installing $(SERVICE_NAME) ---"
	
	@echo "[1] Creating service user '$(SERVICE_USER)' if missing..."
	@id -u $(SERVICE_USER) >/dev/null 2>&1 || useradd -r -s /bin/false $(SERVICE_USER)

	@echo "[2] Creating installation directory..."
	@# 'install -d' creates the directory with specific owner/group
	@install -d -m 755 -o $(SERVICE_USER) -g $(SERVICE_USER) $(INSTALL_DIR)

	@echo "[3] Installing binary..."
	@# 'install' copies the file and sets permissions/owner in one step
	@install -m 755 -o $(SERVICE_USER) -g $(SERVICE_USER) $(TARGET) $(EXECUTABLE)

	@echo "[4] Creating systemd service file..."
	@echo "$$SERVICE_CONTENT" > $(SERVICE_FILE)

	@echo "[5] Reloading and starting service..."
	@systemctl daemon-reload
	@systemctl enable $(SERVICE_NAME)
	@systemctl restart $(SERVICE_NAME)
	
	@echo "--- Installation Complete! ---"

uninstall:
	@echo "--- Uninstalling $(SERVICE_NAME) ---"
	-systemctl stop $(SERVICE_NAME)
	-systemctl disable $(SERVICE_NAME)
	-rm -f $(SERVICE_FILE)
	-systemctl daemon-reload
	-rm -rf $(INSTALL_DIR)
	@echo "--- Uninstalled (User '$(SERVICE_USER)' was preserved) ---"

.PHONY: all clean install uninstall
