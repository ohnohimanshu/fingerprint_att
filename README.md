# FingerAttend — Biometric Attendance System

A full-stack IoT attendance system built with an **ESP32 microcontroller**, **R307 optical fingerprint sensor**, and a **Python Flask** web backend. Students mark attendance by placing their finger on the sensor. The system automatically toggles IN/OUT, stores records with IST timestamps, and provides a real-time web dashboard for admins and a self-service portal for students.

---

## Table of Contents

- [Features](#features)
- [System Architecture](#system-architecture)
- [Hardware](#hardware)
  - [Components List](#components-list)
  - [Wiring Diagram](#wiring-diagram)
- [Software Stack](#software-stack)
- [Project Structure](#project-structure)
- [How It Works](#how-it-works)
  - [Enrollment Flow](#enrollment-flow)
  - [Attendance Flow](#attendance-flow)
  - [Email Credentials](#email-credentials)
- [Database Schema](#database-schema)
- [API Reference](#api-reference)
- [Setup & Installation](#setup--installation)
  - [1. Clone & Install Dependencies](#1-clone--install-dependencies)
  - [2. Configure Environment Variables](#2-configure-environment-variables)
  - [3. Run the Server](#3-run-the-server)
  - [4. Flash the ESP32](#4-flash-the-esp32)
- [Web Interface](#web-interface)
- [Default Credentials](#default-credentials)
- [Security Notes](#security-notes)
- [Built By](#built-by)

---

## Features

- **Biometric attendance** — R307 fingerprint sensor handles all matching on-device; no raw biometric data ever touches the server
- **Auto IN/OUT toggle** — first scan of the day = IN, next scan = OUT, alternating
- **Remote enrollment** — admin triggers fingerprint enrollment from the web dashboard; ESP32 picks it up over Wi-Fi within 2 seconds
- **Real-time OLED feedback** — student name and action displayed on a 128×64 OLED immediately after scan
- **Audio feedback** — 1 beep for IN, 2 beeps for OUT, 3 beeps for errors
- **Admin dashboard** — manage students, view today's attendance count, enroll/edit/delete
- **Student portal** — students log in to view their full attendance history grouped by date
- **Auto-generated credentials** — username and password created automatically when a student is added
- **Email delivery** — credentials sent to the student's email via SMTP on account creation, with a resend option
- **Date-filtered attendance log** — admin can browse any date's records
- **IST timezone** — all timestamps stored and displayed in Indian Standard Time
- **Environment-based config** — all secrets loaded from `.env`, never hardcoded

---

## System Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Local Wi-Fi Network                           │
│                                                                     │
│  ┌──────────────────────────┐   HTTP/JSON   ┌───────────────────┐  │
│  │      ESP32 Device        │◄─────────────►│   Flask Server    │  │
│  │                          │               │   Python 3.11     │  │
│  │  ┌────────────────────┐  │               │   Port 5000       │  │
│  │  │  R307 Fingerprint  │  │               │                   │  │
│  │  │  Sensor (UART)     │  │               │  ┌─────────────┐  │  │
│  │  └────────────────────┘  │               │  │  SQLite DB  │  │  │
│  │  ┌────────────────────┐  │               │  │ attendance  │  │  │
│  │  │  SSD1306 OLED      │  │               │  │    .db      │  │  │
│  │  │  128×64 (I2C)      │  │               │  └─────────────┘  │  │
│  │  └────────────────────┘  │               └────────┬──────────┘  │
│  │  ┌────────────────────┐  │                        │             │
│  │  │  Active Buzzer     │  │               ┌────────▼──────────┐  │
│  │  └────────────────────┘  │               │   Web Browser     │  │
│  └──────────────────────────┘               │                   │  │
│                                             │  Admin Dashboard  │  │
│                                             │  Student Portal   │  │
│                                             └───────────────────┘  │
│                                                        │            │
│                                             ┌──────────▼────────┐  │
│                                             │   Gmail SMTP      │  │
│                                             │  (Credentials     │  │
│                                             │   Email Delivery) │  │
│                                             └───────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
```

### Request Flow — Attendance Marking

```
Student places finger
        │
        ▼
R307 captures image → image2Tz → fingerSearch
        │
        ▼ (match found)
ESP32 sends POST /api/mark-attendance
  { fingerprint_id, confidence }
        │
        ▼
Flask looks up student by fingerprint_id
Flask checks last action → toggles IN/OUT
Flask writes AttendanceRecord to SQLite
Flask returns { name, action, time }
        │
        ▼
ESP32 displays name + action on OLED
Buzzer beeps (1 = IN, 2 = OUT)
```

### Request Flow — Remote Enrollment

```
Admin clicks "Enroll Finger" on dashboard
        │
        ▼
POST /admin/enroll-fingerprint/<id>
Flask assigns next free R307 slot (1–127)
Flask writes slot to student.fingerprint_id
Flask queues ENROLL command (thread-safe)
        │
        ▼ (ESP32 polls every 2s)
GET /api/esp32/command → { command: "ENROLL", fingerprint_id: N }
        │
        ▼
ESP32 runs two-scan enrollment sequence
R307 stores template in slot N
        │
        ▼
POST /api/esp32/enroll-result { fingerprint_id, success }
Flask sets student.is_enrolled = true
Dashboard auto-refreshes → badge turns green
```

---

## Hardware

### Components List

| Component | Model | Qty |
|---|---|---|
| Microcontroller | ESP32 DevKit V1 (38-pin) | 1 |
| Fingerprint Sensor | R307 Optical (UART, 3.3V) | 1 |
| OLED Display | SSD1306 128×64 I2C (0x3C) | 1 |
| Buzzer | Active Buzzer Module (5V) | 1 |
| Power | USB 5V via ESP32 DevKit | — |
| Wires | Dupont jumper wires | — |

---

### Wiring Diagram

```
                        ESP32 DevKit V1
                   ┌─────────────────────┐
                   │                     │
    3.3V ──────────┤ 3V3             GND ├────────── GND (common)
                   │                     │
                   │                     │
    ┌──────────────┤ GPIO16 (RX2)        │
    │  R307        │                     │
    │  Sensor      │ GPIO17 (TX2) ───────┤──────────┐
    │              │                     │          │
    │  VCC ────────┤ 3V3                 │          │
    │  GND ────────┤ GND                 │          │
    │  TX  ────────┘ (GPIO16)            │          │
    └──────────────── (GPIO17) ──────────┘          │
       RX  ◄──────────────────────────────────────── TX
                   │                     │
                   │                     │
    SSD1306 OLED   │                     │
    VCC ───────────┤ 3V3                 │
    GND ───────────┤ GND                 │
    SDA ───────────┤ GPIO21 (SDA)        │
    SCL ───────────┤ GPIO22 (SCL)        │
                   │                     │
    Active Buzzer  │                     │
    VCC ───────────┤ VIN (5V)            │
    GND ───────────┤ GND                 │
    IO  ───────────┤ GPIO25              │
                   │                     │
                   └─────────────────────┘
```

### Pin Summary

| Signal | ESP32 Pin | Notes |
|---|---|---|
| R307 TX → ESP32 | GPIO 16 | Serial2 RX |
| R307 RX ← ESP32 | GPIO 17 | Serial2 TX |
| R307 VCC | 3.3V | Do NOT use 5V |
| OLED SDA | GPIO 21 | I2C Data |
| OLED SCL | GPIO 22 | I2C Clock |
| OLED VCC | 3.3V | |
| Buzzer IO | GPIO 25 | Active HIGH |
| Buzzer VCC | VIN (5V) | From USB rail |

> **Note:** The R307 sensor operates at 3.3V logic. Connecting it to 5V will damage it. The buzzer module requires 5V for full volume — use the VIN pin which is connected directly to the USB 5V rail on the DevKit.

---

## Software Stack

### Backend
| Technology | Version | Purpose |
|---|---|---|
| Python | 3.11 | Runtime |
| Flask | 3.1.2 | Web framework |
| Flask-SQLAlchemy | 3.1.1 | ORM / database layer |
| Werkzeug | 3.1.3 | Password hashing (PBKDF2) |
| python-dotenv | 1.1.0 | `.env` file loading |
| SQLite | built-in | Persistent storage |
| smtplib | built-in | SMTP email delivery |
| zoneinfo | built-in | IST timezone handling |

### Frontend
| Technology | Purpose |
|---|---|
| Jinja2 | Server-side HTML templating |
| Vanilla CSS | Dark theme UI with CSS custom properties |
| Vanilla JS (fetch API) | Async enrollment polling, copy-to-clipboard |
| Space Grotesk + JetBrains Mono | Google Fonts |

### Firmware (ESP32)
| Library | Purpose |
|---|---|
| Adafruit Fingerprint Sensor Library | R307 UART communication |
| Adafruit SSD1306 + GFX | OLED rendering |
| ArduinoJson | JSON serialization for HTTP payloads |
| WiFi + HTTPClient | Built-in ESP32 networking |

---

## Project Structure

```
fingerprint_att/
│
├── app.py                          # Flask app — routes, models, API, email
├── requirements.txt                # Python dependencies (pinned)
├── .env                            # Secrets — SMTP credentials, secret key
├── .gitignore                      # Excludes .env and .db from git
├── attendance.db                   # SQLite database (auto-created on first run)
│
├── esp32_attendance/
│   └── esp32_attendance.ino        # Arduino firmware for ESP32
│
└── templates/
    ├── admin_login.html            # Admin sign-in page
    ├── admin_dashboard.html        # Student table, stats, enroll/edit/delete
    ├── add_student.html            # Student registration + credential display
    ├── attendance_log.html         # Date-filtered attendance records
    ├── student_login.html          # Student sign-in page
    └── student_dashboard.html      # Personal attendance history
```

---

## How It Works

### Enrollment Flow

1. Admin opens the dashboard and clicks **Enroll Finger** next to a student
2. Flask finds the next free slot in the R307 (slots 1–127), saves it to `student.fingerprint_id`, and queues an `ENROLL` command in memory (protected by a `threading.Lock`)
3. The ESP32 polls `/api/esp32/command` every 2 seconds — it picks up the command and enters enrollment mode
4. The OLED prompts the student to place their finger twice
5. The R307 captures two images, creates a combined model, and stores it in the assigned slot
6. The ESP32 POSTs the result to `/api/esp32/enroll-result`
7. Flask sets `student.is_enrolled = True` — the dashboard badge turns green

### Attendance Flow

1. The ESP32 runs in a continuous scan loop when idle
2. On a finger placement, the R307 captures an image, converts it to a template, and searches its internal database
3. On a match, the ESP32 sends `{ fingerprint_id, confidence }` to `/api/mark-attendance`
4. Flask looks up the student, checks their last recorded action, and toggles: last was IN → record OUT, otherwise → record IN
5. The record is saved with a full IST timestamp and a `YYYY-MM-DD` date string for fast filtering
6. Flask returns the student's name and action — the OLED displays it and the buzzer beeps

### Email Credentials

When a student is added with an email address:
- Flask generates a username (`firstname.lastname.rollno` + 3-digit suffix) and a 10-character random password
- `send_credentials_email()` builds an HTML + plain-text MIME email and sends it via Gmail SMTP with STARTTLS
- The credentials box on the page shows ✓ Sent or ✗ Failed
- The admin can click **Resend Email** at any time — this generates a **new** password, updates the database, and resends

---

## Database Schema

### `student`
| Column | Type | Notes |
|---|---|---|
| id | INTEGER PK | Auto-increment |
| name | VARCHAR(120) | Full name |
| roll_no | VARCHAR(30) UNIQUE | Student roll number |
| email | VARCHAR(120) | Optional, for credential delivery |
| course | VARCHAR(80) | e.g. B.Tech |
| branch | VARCHAR(80) | e.g. Computer Science |
| year | VARCHAR(10) | e.g. 2nd Year |
| fingerprint_id | INTEGER UNIQUE | R307 slot (1–127), NULL until enrolled |
| username | VARCHAR(60) UNIQUE | Auto-generated login username |
| password_hash | VARCHAR(256) | PBKDF2-SHA256 hash |
| enrolled_at | DATETIME | IST timestamp of account creation |
| is_enrolled | BOOLEAN | True after successful fingerprint enrollment |

### `attendance_record`
| Column | Type | Notes |
|---|---|---|
| id | INTEGER PK | Auto-increment |
| student_id | INTEGER FK | References student.id |
| action | VARCHAR(3) | "IN" or "OUT" |
| timestamp | DATETIME | Full IST datetime |
| date | VARCHAR(10) | YYYY-MM-DD for fast date filtering |

### `admin`
| Column | Type | Notes |
|---|---|---|
| id | INTEGER PK | Auto-increment |
| username | VARCHAR(60) UNIQUE | Admin login name |
| password_hash | VARCHAR(256) | PBKDF2-SHA256 hash |

---

## API Reference

### Web Routes

| Method | Route | Auth | Description |
|---|---|---|---|
| GET/POST | `/` | — | Student login |
| GET | `/student/dashboard` | Student | Personal attendance history |
| GET | `/student/logout` | Student | Clear session |
| GET/POST | `/admin/login` | — | Admin login |
| GET | `/admin` | Admin | Dashboard — stats + student table |
| GET/POST | `/admin/add-student` | Admin | Register new student |
| POST | `/admin/edit-student/<id>` | Admin | Update student details |
| POST | `/admin/delete-student/<id>` | Admin | Delete student + all records |
| POST | `/admin/enroll-fingerprint/<id>` | Admin | Queue enrollment command |
| POST | `/admin/resend-credentials/<id>` | Admin | Regenerate password + resend email |
| GET | `/admin/attendance` | Admin | Date-filtered attendance log |
| GET | `/admin/logout` | Admin | Clear session |

### ESP32 API Routes

| Method | Route | Auth | Description |
|---|---|---|---|
| POST | `/api/mark-attendance` | — | Record attendance from fingerprint scan |
| GET | `/api/esp32/command` | — | Poll for pending commands (ENROLL) |
| POST | `/api/esp32/enroll-result` | — | Report enrollment success/failure |
| GET | `/api/enrollment-status/<id>` | Admin | Check if student is enrolled |

---

## Setup & Installation

### 1. Clone & Install Dependencies

```bash
git clone https://github.com/your-username/fingerprint_att.git
cd fingerprint_att
pip install -r requirements.txt
```

### 2. Configure Environment Variables

Copy `.env` and fill in your values:

```env
SECRET_KEY=your-random-secret-key

SMTP_HOST=smtp.gmail.com
SMTP_PORT=587
SMTP_USER=your_email@gmail.com
SMTP_PASSWORD=your_16_char_app_password
SMTP_FROM=your_email@gmail.com
```

> For Gmail, you need a **16-character App Password** — not your regular password.
> Go to: Google Account → Security → 2-Step Verification → App Passwords

### 3. Run the Server

```bash
python app.py
```

The server starts on `http://0.0.0.0:5000`. On first run it creates `attendance.db` and a default admin account.

Find your machine's local IP:
```bash
# Windows
ipconfig

# Linux / macOS
ip addr
```

### 4. Flash the ESP32

1. Open `esp32_attendance/esp32_attendance.ino` in Arduino IDE
2. Install required libraries via **Tools → Manage Libraries**:
   - `Adafruit Fingerprint Sensor Library`
   - `Adafruit SSD1306`
   - `Adafruit GFX Library`
   - `ArduinoJson`
3. Edit the constants at the top of the file:

```cpp
const char* WIFI_SSID     = "YourNetworkName";
const char* WIFI_PASSWORD = "YourPassword";
const char* SERVER_URL    = "http://192.168.x.x:5000";  // your PC's local IP
```

4. Select board: **ESP32 Dev Module** and the correct COM port
5. Click **Upload**

---

## Web Interface

| Page | URL | Who |
|---|---|---|
| Student Login | `http://<ip>:5000/` | Students |
| Student Dashboard | `http://<ip>:5000/student/dashboard` | Students |
| Admin Login | `http://<ip>:5000/admin/login` | Admin |
| Admin Dashboard | `http://<ip>:5000/admin` | Admin |
| Add Student | `http://<ip>:5000/admin/add-student` | Admin |
| Attendance Log | `http://<ip>:5000/admin/attendance` | Admin |

---

## Default Credentials

| Role | Username | Password |
|---|---|---|
| Admin | `admin` | `admin123` |

> Change the admin password immediately after first login.

---

## Security Notes

- Passwords are hashed with **PBKDF2-SHA256** via Werkzeug — never stored in plaintext
- All secrets are loaded from `.env` — the file is excluded from git via `.gitignore`
- Route access is enforced by `@admin_required` and `@student_required` decorators
- SQLAlchemy ORM prevents SQL injection via parameterized queries
- Fingerprint templates are stored **on the R307 sensor hardware** — the server only stores a slot number (integer)
- The Flask development server is single-threaded and not suitable for production. For production use, run behind **Gunicorn + Nginx** with HTTPS enabled

---

## Built By

**Himanshu** — designed and built the full system end-to-end:

- Assembled the hardware (ESP32 + R307 + OLED + Buzzer)
- Wrote the Arduino firmware for fingerprint scanning, enrollment, OLED display, and HTTP communication
- Built the Flask backend with SQLAlchemy models, session-based auth, REST API for the ESP32, and SMTP email delivery
- Designed the dark-theme web UI with vanilla CSS and JavaScript
- Integrated the full enrollment pipeline between the web dashboard and the physical sensor over Wi-Fi
