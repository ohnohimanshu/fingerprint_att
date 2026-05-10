# AWS EC2 Deployment Guide — FingerAttend

## Architecture Overview

```
Internet
    │
    │  HTTP (port 80 / 443)
    ▼
┌─────────────────────────────┐
│   AWS EC2 Instance          │
│   Ubuntu 22.04              │
│                             │
│   Nginx (reverse proxy)     │
│       ↕ port 80/443         │
│   Gunicorn (WSGI server)    │
│       ↕ unix socket         │
│   Flask app (app.py)        │
│   SQLite (attendance.db)    │
└─────────────────────────────┘
    ▲
    │  Outbound HTTP only
    │  (ESP32 always initiates)
    │
┌───┴──────────────────────────┐
│  ESP32 (any WiFi network)    │
│  POST /api/mark-attendance   │
│  GET  /api/esp32/command     │
│  POST /api/esp32/enroll-result│
└──────────────────────────────┘
```

The ESP32 only ever makes **outbound** requests to the EC2 public IP.
EC2 never needs to reach the ESP32. This works from any NAT/private network.

---

## EC2 Security Group Rules

In the AWS Console → EC2 → Security Groups → Inbound Rules:

| Type       | Protocol | Port | Source    | Purpose                  |
|------------|----------|------|-----------|--------------------------|
| SSH        | TCP      | 22   | Your IP   | Server management        |
| HTTP       | TCP      | 80   | 0.0.0.0/0 | Web + ESP32 API          |
| HTTPS      | TCP      | 443  | 0.0.0.0/0 | Web + ESP32 API (SSL)    |

> Do NOT open port 5000 publicly. Gunicorn runs on a Unix socket behind Nginx.

---

## Server Setup

### 1. Connect to EC2

```bash
ssh -i your-key.pem ubuntu@YOUR_EC2_PUBLIC_IP
```

### 2. Install system dependencies

```bash
sudo apt update && sudo apt upgrade -y
sudo apt install -y python3-pip python3-venv nginx git
```

### 3. Clone and set up the project

```bash
cd /home/ubuntu
git clone https://github.com/your-username/fingerprint_att.git
cd fingerprint_att

python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
pip install gunicorn
```

### 4. Configure environment variables

```bash
nano .env
```

Fill in all values — especially set a strong `SECRET_KEY` and `ESP32_API_KEY`:

```env
SECRET_KEY=generate-a-long-random-string-here
ESP32_API_KEY=another-long-random-string-here

SMTP_HOST=smtp.gmail.com
SMTP_PORT=587
SMTP_USER=your_email@gmail.com
SMTP_PASSWORD=your_16_char_app_password
SMTP_FROM=your_email@gmail.com
```

Generate strong keys:
```bash
python3 -c "import secrets; print(secrets.token_hex(32))"
```

### 5. Test Gunicorn manually

```bash
source venv/bin/activate
gunicorn --bind 0.0.0.0:5000 app:app
```

Visit `http://YOUR_EC2_IP:5000` — if it loads, Gunicorn is working. Stop it with Ctrl+C.

### 6. Create a systemd service

```bash
sudo nano /etc/systemd/system/fingerattend.service
```

Paste:

```ini
[Unit]
Description=FingerAttend Flask App
After=network.target

[Service]
User=ubuntu
WorkingDirectory=/home/ubuntu/fingerprint_att
Environment="PATH=/home/ubuntu/fingerprint_att/venv/bin"
EnvironmentFile=/home/ubuntu/fingerprint_att/.env
ExecStart=/home/ubuntu/fingerprint_att/venv/bin/gunicorn \
    --workers 2 \
    --bind unix:/home/ubuntu/fingerprint_att/fingerattend.sock \
    --access-logfile /home/ubuntu/fingerprint_att/access.log \
    --error-logfile /home/ubuntu/fingerprint_att/error.log \
    app:app
Restart=always

[Install]
WantedBy=multi-user.target
```

Enable and start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable fingerattend
sudo systemctl start fingerattend
sudo systemctl status fingerattend
```

### 7. Configure Nginx

```bash
sudo nano /etc/nginx/sites-available/fingerattend
```

Paste:

```nginx
server {
    listen 80;
    server_name YOUR_EC2_PUBLIC_IP;  # or your domain name

    # Increase timeout for long-running enrollment polls
    proxy_read_timeout 120s;
    proxy_connect_timeout 10s;

    location / {
        proxy_pass         http://unix:/home/ubuntu/fingerprint_att/fingerattend.sock;
        proxy_set_header   Host $host;
        proxy_set_header   X-Real-IP $remote_addr;
        proxy_set_header   X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header   X-Forwarded-Proto $scheme;
    }
}
```

Enable and reload:

```bash
sudo ln -s /etc/nginx/sites-available/fingerattend /etc/nginx/sites-enabled/
sudo nginx -t
sudo systemctl reload nginx
```

---

## ESP32 Configuration

Edit the top of `esp32_attendance.ino`:

```cpp
const char* WIFI_SSID     = "YourWiFiName";
const char* WIFI_PASSWORD = "YourWiFiPassword";
const char* SERVER_URL    = "http://YOUR_EC2_PUBLIC_IP";  // no trailing slash
const char* API_KEY       = "your-secret-api-key-here";  // must match .env ESP32_API_KEY
```

Flash the firmware and open Serial Monitor at 115200 baud to verify connection.

---

## Test with curl

Replace `YOUR_EC2_IP` and `YOUR_API_KEY` with your actual values.

### Mark attendance (simulate ESP32 scan)
```bash
curl -X POST http://YOUR_EC2_IP/api/mark-attendance \
  -H "Content-Type: application/json" \
  -H "X-API-Key: YOUR_API_KEY" \
  -d '{"fingerprint_id": 1, "confidence": 95}'
```

Expected response:
```json
{"ok": true, "name": "Student Name", "action": "IN", "time": "10:30"}
```

### Poll for enrollment command
```bash
curl http://YOUR_EC2_IP/api/esp32/command \
  -H "X-API-Key: YOUR_API_KEY"
```

### Report enrollment result
```bash
curl -X POST http://YOUR_EC2_IP/api/esp32/enroll-result \
  -H "Content-Type: application/json" \
  -H "X-API-Key: YOUR_API_KEY" \
  -d '{"fingerprint_id": 1, "success": true}'
```

### Test unauthorized access (should return 401)
```bash
curl http://YOUR_EC2_IP/api/esp32/command
```

---

## Updating the App

```bash
cd /home/ubuntu/fingerprint_att
git pull
source venv/bin/activate
pip install -r requirements.txt
sudo systemctl restart fingerattend
```

---

## Logs

```bash
# App logs
tail -f /home/ubuntu/fingerprint_att/error.log
tail -f /home/ubuntu/fingerprint_att/access.log

# Service status
sudo systemctl status fingerattend

# Nginx logs
sudo tail -f /var/log/nginx/error.log
```

---

## Optional: Add HTTPS with Let's Encrypt

If you have a domain name pointed at your EC2 IP:

```bash
sudo apt install certbot python3-certbot-nginx -y
sudo certbot --nginx -d yourdomain.com
```

Then update `SERVER_URL` in the ESP32 firmware to `https://yourdomain.com`.
