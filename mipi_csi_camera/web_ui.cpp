/**
 * Web UI - HTML interface for camera settings and stream control
 * Phase 9: Settings page with resolution selector and quality controls
 */

#include "web_ui.h"

const char* WebUI::getSettingsPage() {
  return R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Camera Settings</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #2193b0 0%, #6dd5ed 100%);
            min-height: 100vh;
            padding: 20px;
        }
        .container {
            max-width: 1200px;
            margin: 0 auto;
        }
        .header {
            background: white;
            border-radius: 10px;
            box-shadow: 0 5px 20px rgba(0,0,0,0.1);
            padding: 20px 30px;
            margin-bottom: 20px;
            display: flex;
            justify-content: space-between;
            align-items: center;
        }
        h1 {
            color: #333;
            font-size: 28px;
            display: flex;
            align-items: center;
            gap: 10px;
        }
        .grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 20px;
            margin-bottom: 20px;
        }
        @media (max-width: 768px) {
            .grid {
                grid-template-columns: 1fr;
            }
        }
        .card {
            background: white;
            border-radius: 10px;
            box-shadow: 0 5px 20px rgba(0,0,0,0.1);
            padding: 30px;
        }
        .card h2 {
            color: #333;
            margin-bottom: 20px;
            font-size: 20px;
            border-bottom: 2px solid #2193b0;
            padding-bottom: 10px;
        }
        .form-group {
            margin-bottom: 20px;
        }
        label {
            display: block;
            margin-bottom: 8px;
            color: #555;
            font-weight: 500;
            font-size: 14px;
        }
        select, input[type="range"] {
            width: 100%;
            padding: 10px;
            border: 2px solid #ddd;
            border-radius: 5px;
            font-size: 14px;
            transition: border-color 0.3s;
        }
        select:focus {
            outline: none;
            border-color: #2193b0;
        }
        input[type="range"] {
            padding: 0;
            height: 40px;
        }
        .slider-value {
            display: inline-block;
            background: #2193b0;
            color: white;
            padding: 4px 12px;
            border-radius: 4px;
            font-weight: 600;
            font-size: 14px;
            margin-left: 10px;
        }
        .button-group {
            display: flex;
            gap: 10px;
            margin-top: 20px;
        }
        button {
            flex: 1;
            padding: 12px;
            border: none;
            border-radius: 5px;
            font-size: 14px;
            font-weight: 600;
            cursor: pointer;
            transition: all 0.3s;
        }
        .btn-primary {
            background: #2193b0;
            color: white;
        }
        .btn-primary:hover {
            background: #1a7a94;
            transform: translateY(-2px);
            box-shadow: 0 5px 15px rgba(33, 147, 176, 0.4);
        }
        .btn-secondary {
            background: #6c757d;
            color: white;
        }
        .btn-secondary:hover {
            background: #5a6268;
        }
        .btn-danger {
            background: #dc3545;
            color: white;
        }
        .btn-danger:hover {
            background: #c82333;
        }
        .btn-logout {
            flex: 0 0 auto;
            padding: 8px 16px;
            background: #6c757d;
            color: white;
            border: none;
            border-radius: 5px;
            cursor: pointer;
            font-size: 13px;
            font-weight: 600;
            transition: all 0.3s;
            width: auto;
            min-width: 80px;
        }
        .btn-logout:hover {
            background: #5a6268;
            transform: translateY(-1px);
        }
        .status-item {
            display: flex;
            justify-content: space-between;
            padding: 12px 0;
            border-bottom: 1px solid #f0f0f0;
        }
        .status-item:last-child {
            border-bottom: none;
        }
        .status-label {
            color: #666;
            font-weight: 500;
        }
        .status-value {
            color: #333;
            font-family: 'Courier New', monospace;
            font-weight: 600;
        }
        .preview-box {
            background: #000;
            border-radius: 5px;
            overflow: hidden;
            position: relative;
            aspect-ratio: 1/1;
            display: flex;
            align-items: center;
            justify-content: center;
        }
        .preview-box img {
            max-width: 100%;
            max-height: 100%;
            object-fit: contain;
        }
        .preview-placeholder {
            color: #666;
            font-size: 14px;
        }
        .message {
            padding: 12px;
            margin-bottom: 20px;
            border-radius: 5px;
            text-align: center;
            display: none;
        }
        .message.success {
            background: #d4edda;
            color: #155724;
            border: 1px solid #c3e6cb;
        }
        .message.error {
            background: #f8d7da;
            color: #721c24;
            border: 1px solid #f5c6cb;
        }
        .message.show {
            display: block;
        }
        .hint {
            font-size: 12px;
            color: #999;
            margin-top: 4px;
            font-style: italic;
        }
        .resolution-info {
            background: #f9f9f9;
            padding: 10px;
            border-radius: 5px;
            font-size: 13px;
            color: #666;
            margin-top: 10px;
        }
        .auto-refresh {
            display: flex;
            align-items: center;
            gap: 8px;
            margin-top: 10px;
        }
        .auto-refresh input[type="checkbox"] {
            width: auto;
            cursor: pointer;
        }
        .auto-refresh label {
            margin: 0;
            cursor: pointer;
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>📷 Camera Settings</h1>
            <button class="btn-logout" onclick="logout()">Logout</button>
        </div>

        <div id="message" class="message"></div>

        <div class="grid">
            <!-- Settings Panel -->
            <div class="card">
                <h2>Stream Configuration</h2>
                <form id="settingsForm">
                    <div class="form-group">
                        <label for="resolution">Resolution:</label>
                        <select id="resolution" name="resolution">
                            <option value="0">WVGA (800x640)</option>
                            <option value="1">XVGA (800x800)</option>
                            <option value="2">Portrait (800x1280)</option>
                        </select>
                        <div class="hint">Select the camera resolution for video stream</div>
                        <div class="resolution-info" id="resolutionInfo">
                            Current: <span id="currentRes">Loading...</span>
                        </div>
                    </div>

                    <div class="form-group">
                        <label for="quality">JPEG Quality: <span class="slider-value" id="qualityValue">50</span></label>
                        <input type="range" id="quality" name="quality" min="10" max="100" value="50" step="5">
                        <div class="hint">Higher quality = larger file size (10-100)</div>
                    </div>

                    <div class="button-group">
                        <button type="submit" class="btn-primary">Apply Settings</button>
                        <button type="button" class="btn-secondary" onclick="loadSettings()">Refresh</button>
                    </div>

                    <div class="button-group">
                        <button type="button" class="btn-danger" onclick="resetSettings()">Reset to Defaults</button>
                    </div>
                </form>
            </div>

            <!-- Live Preview Panel -->
            <div class="card">
                <h2>Live Preview</h2>
                <div class="preview-box" id="previewBox">
                    <img id="previewImage" alt="Camera Preview" style="display:none;">
                    <div class="preview-placeholder" id="previewPlaceholder">Click "Capture" to preview</div>
                </div>
                <div class="auto-refresh">
                    <input type="checkbox" id="autoRefresh">
                    <label for="autoRefresh">Auto-refresh (every 2s)</label>
                </div>
                <div class="button-group" style="margin-top: 15px;">
                    <button type="button" class="btn-primary" onclick="capturePhoto()">Capture</button>
                    <button type="button" class="btn-secondary" onclick="openStream()">Open Stream</button>
                </div>
            </div>
        </div>

        <!-- Status Panel -->
        <div class="card">
            <h2>Device Status</h2>
            <div class="status-item">
                <span class="status-label">Resolution:</span>
                <span class="status-value" id="statusRes">Loading...</span>
            </div>
            <div class="status-item">
                <span class="status-label">JPEG Quality:</span>
                <span class="status-value" id="statusQuality">Loading...</span>
            </div>
            <div class="status-item">
                <span class="status-label">IP Address:</span>
                <span class="status-value" id="statusIP">Loading...</span>
            </div>
            <div class="status-item">
                <span class="status-label">RTSP URL:</span>
                <span class="status-value" id="statusRTSP">Loading...</span>
            </div>
        </div>
    </div>

    <script>
        let autoRefreshInterval = null;
        let captureInProgress = false;

        // Update quality slider value display
        document.getElementById('quality').addEventListener('input', function() {
            document.getElementById('qualityValue').textContent = this.value;
        });

        // Auto-refresh checkbox handler
        document.getElementById('autoRefresh').addEventListener('change', function() {
            if (this.checked) {
                autoRefreshInterval = setInterval(capturePhoto, 2000);
                capturePhoto(); // Immediate capture
            } else {
                if (autoRefreshInterval) {
                    clearInterval(autoRefreshInterval);
                    autoRefreshInterval = null;
                }
            }
        });

        async function loadSettings() {
            try {
                const response = await fetch('/api/settings');
                if (!response.ok) throw new Error('Failed to load settings');

                const data = await response.json();

                // Update form fields
                document.getElementById('resolution').value = data.stream_resolution || '1';
                document.getElementById('quality').value = data.jpeg_quality || '50';
                document.getElementById('qualityValue').textContent = data.jpeg_quality || '50';

                // Update status display
                const resNames = ['WVGA (800x640)', 'XVGA (800x800)', 'Portrait (800x1280)'];
                const resIndex = data.stream_resolution || 1;
                document.getElementById('currentRes').textContent = resNames[resIndex];
                document.getElementById('statusRes').textContent = resNames[resIndex];
                document.getElementById('statusQuality').textContent = data.jpeg_quality || '50';

                showMessage('Settings loaded successfully', 'success');
            } catch (error) {
                showMessage('Failed to load settings: ' + error.message, 'error');
            }

            // Load stream resolution info
            try {
                const response = await fetch('/api/stream/resolution');
                if (response.ok) {
                    const data = await response.json();
                    if (data.width && data.height) {
                        document.getElementById('statusRes').textContent =
                            data.resolution_name + ' (' + data.width + 'x' + data.height + ')';
                    }
                }
            } catch (error) {
                console.log('Could not load stream resolution:', error);
            }

            // Update IP and RTSP URL
            updateNetworkInfo();
        }

        async function updateNetworkInfo() {
            // This is a placeholder - in a real implementation, you'd get this from the device
            // For now, we'll construct it from the current page URL
            const hostname = window.location.hostname;
            document.getElementById('statusIP').textContent = hostname;
            document.getElementById('statusRTSP').textContent = 'rtsp://' + hostname + ':554/';
        }

        async function capturePhoto() {
            if (captureInProgress) {
                return;
            }
            captureInProgress = true;
            try {
                // Keep the current photo ID so a completed capture can be
                // distinguished from the photo that was already displayed.
                let previousPhotoId = null;
                try {
                    const metadataResponse = await fetch('/api/photo/metadata', {
                        cache: 'no-store'
                    });
                    if (metadataResponse.ok) {
                        const metadata = await metadataResponse.json();
                        previousPhotoId = metadata.id || null;
                    }
                } catch (error) {
                    // A first capture has no metadata yet; polling below will
                    // handle that expected 404 response.
                }

                const response = await fetch('/api/photo/capture', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json'
                    },
                    body: '{}'
                });

                if (!response.ok) throw new Error('Capture failed');

                const data = await response.json();

                if (data.status === 'success' || data.status === 'capturing') {
                    const photoId = await waitForCapturedPhoto(previousPhotoId);
                    const img = document.getElementById('previewImage');
                    const placeholder = document.getElementById('previewPlaceholder');

                    img.onload = function() {
                        img.style.display = 'block';
                        placeholder.style.display = 'none';
                        if (!document.getElementById('autoRefresh').checked) {
                            showMessage('Photo captured successfully', 'success');
                        }
                    };

                    img.onerror = function() {
                        img.style.display = 'none';
                        placeholder.style.display = 'block';
                        placeholder.textContent = 'Photo not ready yet, try again';
                        if (!document.getElementById('autoRefresh').checked) {
                            showMessage('Photo not ready, please try again', 'error');
                        }
                    };

                    // The API accepts a photo ID query parameter. Using the
                    // published ID also gives each capture a unique URL.
                    img.src = '/api/photo/latest.jpg?id=' + encodeURIComponent(photoId);
                } else {
                    showMessage('Capture failed: ' + (data.message || 'Unknown error'), 'error');
                }
            } catch (error) {
                showMessage('Error capturing photo: ' + error.message, 'error');
            } finally {
                captureInProgress = false;
            }
        }

        async function waitForCapturedPhoto(previousPhotoId) {
            const maxAttempts = 24;
            for (let attempt = 0; attempt < maxAttempts; attempt++) {
                await new Promise(resolve => setTimeout(resolve, 500));
                try {
                    const response = await fetch('/api/photo/metadata', {
                        cache: 'no-store'
                    });
                    if (!response.ok) {
                        continue;
                    }

                    const metadata = await response.json();
                    if (metadata.state === 'error') {
                        throw new Error('Capture failed on camera');
                    }
                    if (metadata.state === 'ready' && metadata.id
                        && metadata.id !== previousPhotoId) {
                        return metadata.id;
                    }
                } catch (error) {
                    if (error.message === 'Capture failed on camera') {
                        throw error;
                    }
                }
            }
            throw new Error('Photo not ready, please try again');
        }

        function openStream() {
            const hostname = window.location.hostname;
            const rtspUrl = 'rtsp://' + hostname + ':554/';

            // Copy to clipboard if supported
            if (navigator.clipboard && navigator.clipboard.writeText) {
                navigator.clipboard.writeText(rtspUrl).then(() => {
                    showMessage('RTSP URL copied to clipboard: ' + rtspUrl, 'success');
                }).catch(() => {
                    showMessage('RTSP URL: ' + rtspUrl + ' (copy manually)', 'success');
                });
            } else {
                // Fallback for browsers without clipboard API
                showMessage('RTSP URL: ' + rtspUrl + ' (copy manually)', 'success');
            }
        }

        document.getElementById('settingsForm').addEventListener('submit', async (e) => {
            e.preventDefault();

            const resolution = parseInt(document.getElementById('resolution').value);
            const quality = parseInt(document.getElementById('quality').value);

            try {
                const response = await fetch('/api/settings', {
                    method: 'PUT',
                    headers: {
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify({
                        stream_resolution: resolution,
                        jpeg_quality: quality
                    })
                });

                if (!response.ok) throw new Error('Failed to save settings');

                const data = await response.json();

                if (data.status === 'success') {
                    showMessage('Settings applied successfully!', 'success');
                    await loadSettings();
                } else {
                    showMessage('Failed to apply settings: ' + (data.message || 'Unknown error'), 'error');
                }
            } catch (error) {
                showMessage('Error saving settings: ' + error.message, 'error');
            }
        });

        async function resetSettings() {
            if (!confirm('Reset all settings to defaults?')) return;

            try {
                const response = await fetch('/api/settings/reset', {
                    method: 'POST'
                });

                if (!response.ok) throw new Error('Failed to reset settings');

                const data = await response.json();

                if (data.status === 'success') {
                    showMessage('Settings reset to defaults', 'success');
                    await loadSettings();
                } else {
                    showMessage('Failed to reset settings', 'error');
                }
            } catch (error) {
                showMessage('Error resetting settings: ' + error.message, 'error');
            }
        }

        async function logout() {
            // For now, just redirect to root
            // In a real implementation, you'd call a logout API
            window.location.href = '/';
        }

        function showMessage(text, type) {
            const msgDiv = document.getElementById('message');
            msgDiv.textContent = text;
            msgDiv.className = 'message show ' + type;
            setTimeout(() => {
                msgDiv.classList.remove('show');
            }, 4000);
        }

        // Load settings on page load
        window.addEventListener('load', loadSettings);
    </script>
</body>
</html>
)HTML";
}

const char* WebUI::getLoginPage() {
  return R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Camera Login</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #2193b0 0%, #6dd5ed 100%);
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
            padding: 20px;
        }
        .login-container {
            background: white;
            padding: 40px;
            border-radius: 10px;
            box-shadow: 0 10px 25px rgba(0, 0, 0, 0.2);
            width: 100%;
            max-width: 400px;
        }
        h1 {
            text-align: center;
            color: #333;
            margin-bottom: 30px;
            font-size: 28px;
        }
        .camera-icon {
            text-align: center;
            font-size: 48px;
            margin-bottom: 20px;
        }
        .form-group {
            margin-bottom: 20px;
        }
        label {
            display: block;
            margin-bottom: 8px;
            color: #555;
            font-weight: 500;
        }
        input[type="text"],
        input[type="password"] {
            width: 100%;
            padding: 12px;
            border: 2px solid #ddd;
            border-radius: 5px;
            font-size: 16px;
            transition: border-color 0.3s;
        }
        input[type="text"]:focus,
        input[type="password"]:focus {
            outline: none;
            border-color: #2193b0;
        }
        .btn-login {
            width: 100%;
            padding: 12px;
            background: linear-gradient(135deg, #2193b0 0%, #6dd5ed 100%);
            color: white;
            border: none;
            border-radius: 5px;
            font-size: 16px;
            font-weight: 600;
            cursor: pointer;
            transition: transform 0.2s;
        }
        .btn-login:hover {
            transform: translateY(-2px);
        }
        .btn-login:active {
            transform: translateY(0);
        }
        .message {
            padding: 12px;
            border-radius: 5px;
            margin-bottom: 20px;
            text-align: center;
            display: none;
        }
        .message.error {
            background: #fee;
            color: #c33;
            border: 1px solid #fcc;
            display: block;
        }
        .message.success {
            background: #efe;
            color: #3c3;
            border: 1px solid #cfc;
            display: block;
        }
    </style>
</head>
<body>
    <div class="login-container">
        <div class="camera-icon">📷</div>
        <h1>Camera Login</h1>
        <div id="message" class="message"></div>
        <form id="loginForm">
            <div class="form-group">
                <label for="username">Username:</label>
                <input type="text" id="username" name="username" required autofocus>
            </div>
            <div class="form-group">
                <label for="password">Password:</label>
                <input type="password" id="password" name="password" required>
            </div>
            <button type="submit" class="btn-login">Login</button>
        </form>
    </div>
    <script>
        const loginForm = document.getElementById('loginForm');
        const messageDiv = document.getElementById('message');

        loginForm.addEventListener('submit', async (e) => {
            e.preventDefault();

            const username = document.getElementById('username').value;
            const password = document.getElementById('password').value;

            try {
                const response = await fetch('/api/login', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/x-www-form-urlencoded',
                    },
                    body: `username=${encodeURIComponent(username)}&password=${encodeURIComponent(password)}`
                });

                const data = await response.json();

                if (response.ok && data.status === 'success') {
                    messageDiv.className = 'message success';
                    messageDiv.textContent = 'Login successful! Redirecting...';
                    setTimeout(() => {
                        window.location.href = '/';
                    }, 500);
                } else {
                    messageDiv.className = 'message error';
                    messageDiv.textContent = data.message || 'Invalid username or password';
                }
            } catch (error) {
                messageDiv.className = 'message error';
                messageDiv.textContent = 'Login failed. Please try again.';
            }
        });
    </script>
</body>
</html>
)HTML";
}
