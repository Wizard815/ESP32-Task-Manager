import tkinter as tk
from tkinter import ttk, messagebox, filedialog
from tkcalendar import Calendar
import serial, serial.tools.list_ports, json, threading, os, time
from datetime import datetime

# Always store tasks.json in the same folder as this script
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
TASK_FILE = os.path.join(BASE_DIR, "tasks.json")

# ---------------- Globals ----------------
ser = None
running = True
tasks_cache = []   # local mirror of tasks
color_preview = True
screen_on = True
NOTES_MAX_LEN = 180  # max characters for per-task notes

STATUS_COLORS = {
    "In Progress": "#00FF00",
    "Paused": "#F0B400",
    "Waiting on": "#50A0DC",
    "Done": "#14783C",
    "Ready to Ship": "#A05AC8",
}

STATUS_OPTIONS = [
    "In Progress",
    "Paused",
    "Waiting on",
    "Done",
    "Ready to Ship",
]

# ---------------- Persistence ----------------
def load_local_tasks():
    """Load tasks.json if present, else create it."""
    global tasks_cache
    if os.path.exists(TASK_FILE):
        try:
            with open(TASK_FILE, "r", encoding="utf-8") as f:
                data = json.load(f)
            tasks_cache = data.get("tasks", [])
            # Normalize NULL-ish values
            for t in tasks_cache:
                if t.get("status") in ("None", "NULL"):
                    t["status"] = ""
                if t.get("notes") in ("None", "NULL"):
                    t["notes"] = ""
        except Exception:
            tasks_cache = []
    else:
        save_local_tasks()
def save_local_tasks():
    try:
        with open(TASK_FILE, "w", encoding="utf-8") as f:
            json.dump(tasks_cache, f, indent=2)
        print("Saved tasks.json")
    except Exception as e:
        print(f"Failed to save tasks.json: {e}")
# ---------------- Serial ----------------
def serial_reader():
    """Background thread to read incoming lines from ESP."""
    global tasks_cache
    while running:
        try:
            if ser and ser.in_waiting > 0:
                line = ser.readline().decode(errors="ignore").strip()
                if line:
                    log.insert(tk.END, f"<ESP32> {line}\n")
                    log.see(tk.END)
                    # Try to parse incoming JSON
                    try:
                        data = json.loads(line)
                        # ESP sent full task list
                        if "tasks" in data:
                            tasks = data["tasks"]
                            # normalize NULL-ish values
                            for t in tasks:
                                if t.get("status") in ("None", "NULL"):
                                    t["status"] = ""
                                if t.get("notes") in ("None", "NULL"):
                                    t["notes"] = ""
                            tasks_cache = tasks
                            save_local_tasks()
                            refresh_task_listbox()
                    except Exception:
                        # non-JSON diagnostic text from ESP
                        pass
        except Exception:
            pass
        time.sleep(0.05)


def connect_serial():
    """Connect to chosen COM port and sync PC time."""
    global ser
    port = port_combo.get()
    if not port:
        messagebox.showerror("Error", "Select COM port first")
        return
    try:
        ser = serial.Serial(port, 115200, timeout=0.5)
        threading.Thread(target=serial_reader, daemon=True).start()
        messagebox.showinfo("Connected", f"Connected to {port}")

        # Send PC time to ESP for synchronization (hour + minute, if you keep that on ESP)
        now = datetime.now()
        send_json({"cmd": "SET_TIME", "hour": now.hour, "minute": now.minute})

        # Also send epoch so ESP can track live time if it wants
        epoch = int(time.time())
        send_json({"cmd": "TIME", "epoch": epoch})

        # Push local tasks right after sync
        push_local_to_esp()

        # Job Sync
        send_json({"cmd": "LIST_TASKS"})


    except Exception as e:
        messagebox.showerror("Connection Error", str(e))


def send_json(d: dict):
    """Send a single JSON command to ESP."""
    if not ser:
        messagebox.showerror("Error", "Not connected to ESP32")
        return
    try:
        line = json.dumps(d) + "\n"
        ser.write(line.encode())
        log.insert(tk.END, f"<PC> {d}\n")
        log.see(tk.END)
    except Exception as e:
        messagebox.showerror("Serial Write Error", str(e))


def push_local_to_esp():
    """Send local tasks (from tasks.json) to ESP, skipping duplicates."""
    if not tasks_cache:
        log.insert(tk.END, "No local tasks to push.\n")
        return

    log.insert(tk.END, "Pushing local tasks to ESP...\n")

    # Avoid duplicates based on title + month + day
    seen = set()
    for idx, t in enumerate(tasks_cache):
        key = f"{t.get('title','')}_{t.get('month','')}_{t.get('day',0)}"
        if key in seen:
            continue
        seen.add(key)

        payload = {
            "cmd": "ADD_TASK",
            "title": t.get("title", f"Task {idx+1}"),
            "month": t.get("month", "None"),
            "day": t.get("day", 0),
            "priority": t.get("priority", 0),
        }

        # optional fields
        if t.get("time"):
            payload["time"] = t["time"]
        if t.get("status"):
            payload["status"] = t["status"]
        notes_val = t.get("notes", "")
        if notes_val:
            payload["notes"] = str(notes_val)[:NOTES_MAX_LEN]

        send_json(payload)

    # Ask ESP to confirm final list
    send_json({"cmd": "LIST_TASKS"})


# ---------------- Time Sync ----------------
def time_sync_loop():
    """Every hour, send PC time (epoch) to ESP."""
    while running:
        try:
            if ser:
                epoch = int(time.time())
                send_json({
                    "cmd": "TIME",
                    "epoch": epoch
                })
        except Exception:
            pass
        # once per hour
        time.sleep(3600)


def periodic_refresh_loop():
    """Periodically ask ESP for full task list so Python view stays in sync
    even if you move tasks directly on the ESP UI."""
    while running:
        try:
            if ser:
                send_json({"cmd": "LIST_TASKS"})
        except Exception:
            pass
        # once per minute is plenty
        time.sleep(60)


def sync_time():
    """Manual Sync Time button: use same connection + protocol as loop."""
    if not ser:
        messagebox.showerror("Error", "Not connected to ESP32")
        return

    epoch = int(time.time())
    send_json({
        "cmd": "TIME",
        "epoch": epoch
    })

    now = datetime.now()
    messagebox.showinfo(
        "Time Synced",
        f"Sent time {now.strftime('%I:%M %p, %B %d')}"
    )


# ---------------- Task Actions ----------------
def add_task():
    """Create a new task from UI fields and send to ESP, including notes."""
    global tasks_cache

    title = title_entry.get().strip()
    if not title:
        messagebox.showwarning("Missing", "Enter a task title")
        return

    # Date handling
    if no_date_var.get():
        month = "None"
        day = 0
    else:
        sel_date = cal.selection_get()
        month = sel_date.strftime("%B")
        day = sel_date.day

    hour = hour_entry.get().strip()
    minute = min_entry.get().strip()
    ampm = ampm_var.get()
    eod = eod_var.get()

    # Notes (optional, limited to NOTES_MAX_LEN)
    try:
        raw_notes = notes_text.get("1.0", "end").strip()
    except Exception:
        raw_notes = ""
    if len(raw_notes) > NOTES_MAX_LEN:
        raw_notes = raw_notes[:NOTES_MAX_LEN]

    payload = {
        "cmd": "ADD_TASK",
        "title": title,
        "month": month,
        "day": day,
        "priority": 1 if eod else 0,
    }

    new_task = {
        "title": title,
        "month": month,
        "day": day,
        "priority": 1 if eod else 0,
        "status": "",
        "time": "",
        "notes": raw_notes,
    }

    # Only add time if hour+minute given
    if hour and minute:
        if len(minute) == 1:
            minute = "0" + minute
        time_str = f"{hour}:{minute} {ampm}"
        payload["time"] = time_str
        new_task["time"] = time_str

    if raw_notes:
        payload["notes"] = raw_notes

    # Local add + persist
    tasks_cache.append(new_task)
    save_local_tasks()

    # Send to ESP
    send_json(payload)

    # Clear inputs
    title_entry.delete(0, tk.END)
    try:
        notes_text.delete("1.0", "end")
    except Exception:
        pass
    eod_var.set(False)
    no_date_var.set(False)

    # Refresh list
    refresh_task_listbox()


def remove_task():
    """Remove selected task locally and on ESP.

    We use both the local index and any 'id' field the ESP provided, so
    the firmware can choose how to delete without relying on our list order.
    """
    sel = task_listbox.curselection()
    if not sel:
        messagebox.showinfo("No task", "Select a task to remove")
        return

    idx = sel[0]
    if idx < 0 or idx >= len(tasks_cache):
        return

    if not messagebox.askyesno("Confirm", "Delete selected task?"):
        return

    t = tasks_cache[idx]
    esp_id = t.get("id", idx)

    # Remove locally
    tasks_cache.pop(idx)
    save_local_tasks()
    refresh_task_listbox()

    # Tell ESP
    send_json({
        "cmd": "DELETE_TASK",
        "id": esp_id,
        "index": idx
    })


def edit_task():
    """Open edit dialog for selected task (including notes)."""
    sel = task_listbox.curselection()
    if not sel:
        messagebox.showinfo("No task", "Select a task to edit")
        return

    idx = sel[0]
    if idx < 0 or idx >= len(tasks_cache):
        return

    t = tasks_cache[idx]
    esp_id = t.get("id", idx)

    win = tk.Toplevel(root)
    win.title("Edit Task")
    win.geometry("380x600")
    win.grab_set()

    ttk.Label(win, text="Task Title:").pack(anchor="w", padx=10, pady=(8, 3))
    title_var = tk.StringVar(value=t.get("title", ""))
    title_entry_local = ttk.Entry(win, textvariable=title_var, width=36)
    title_entry_local.pack(padx=10, pady=2)

    # Date
    ttk.Label(win, text="Date:").pack(anchor="w", padx=10, pady=(6, 2))
    cal_local = Calendar(win, selectmode="day")
    cal_local.pack(padx=10, pady=5)

    month_val = t.get("month", "None")
    try:
        day_val = int(t.get("day", 0) or 0)
    except Exception:
        day_val = 0

    if month_val not in ("None", "", None) and day_val > 0:
        try:
            cur = datetime.strptime(f"{month_val} {day_val} 2025", "%B %d %Y")
            cal_local.selection_set(cur)
        except Exception:
            pass

    # Time
    frame_time = ttk.LabelFrame(win, text="Time (optional)")
    frame_time.pack(padx=10, pady=6, fill="x")
    hour_local = ttk.Entry(frame_time, width=5)
    min_local = ttk.Entry(frame_time, width=5)
    ampm_local = tk.StringVar(value="PM")
    ttk.Label(frame_time, text="Hr").grid(row=0, column=0, padx=2)
    hour_local.grid(row=0, column=1, padx=2)
    ttk.Label(frame_time, text="Min").grid(row=0, column=2, padx=2)
    min_local.grid(row=0, column=3, padx=2)
    ttk.Radiobutton(frame_time, text="AM", variable=ampm_local, value="AM").grid(
        row=0, column=4, padx=2
    )
    ttk.Radiobutton(frame_time, text="PM", variable=ampm_local, value="PM").grid(
        row=0, column=5, padx=2
    )

    if t.get("time"):
        try:
            parts = t["time"].split()
            hh, mm = parts[0].split(":")
            hour_local.insert(0, hh)
            min_local.insert(0, mm)
            if len(parts) > 1:
                ampm_local.set(parts[1])
        except Exception:
            pass

    # EOD flag
    eod_local = tk.BooleanVar(value=bool(t.get("priority", 0)))
    ttk.Checkbutton(
        win, text="End of Day (priority)", variable=eod_local
    ).pack(anchor="w", padx=10, pady=4)

    # Status
    ttk.Label(win, text="Status:").pack(anchor="w", padx=10, pady=(6, 2))
    status_value = t.get("status", "") or ""
    if status_value in ("None", "NULL"):
        status_value = ""
    status_local = tk.StringVar(value=status_value)
    status_box = ttk.Combobox(
        win,
        values=[""] + STATUS_OPTIONS,
        textvariable=status_local,
        state="readonly",
        width=30,
    )
    status_box.pack(padx=10, pady=3)

    # Notes
    ttk.Label(win, text=f"Notes (max {NOTES_MAX_LEN} chars):").pack(
        anchor="w", padx=10, pady=(8, 2)
    )
    notes_box = tk.Text(win, width=40, height=6, wrap="word")
    notes_box.pack(padx=10, pady=(0, 6))
    existing_notes = t.get("notes", "")
    if existing_notes and existing_notes not in ("NULL", "None"):
        notes_box.insert("1.0", existing_notes)

    def save_changes():
        title_new = title_var.get().strip()
        if not title_new:
            messagebox.showwarning("Missing", "Title required")
            return

        date_sel = cal_local.selection_get()
        month_new = date_sel.strftime("%B")
        day_new = date_sel.day

        hour_new = hour_local.get().strip()
        min_new = min_local.get().strip()
        ampm_new = ampm_local.get()

        time_str = ""
        if hour_new and min_new:
            if len(min_new) == 1:
                min_new = "0" + min_new
            time_str = f"{hour_new}:{min_new} {ampm_new}"

        status_new = status_local.get()
        if status_new in ("None", "NULL"):
            status_new = ""

        notes_new = notes_box.get("1.0", "end").strip()
        if len(notes_new) > NOTES_MAX_LEN:
            notes_new = notes_new[:NOTES_MAX_LEN]

        # Update local copy
        t["title"] = title_new
        t["month"] = month_new
        t["day"] = day_new
        t["priority"] = 1 if eod_local.get() else 0
        t["status"] = status_new
        t["time"] = time_str
        t["notes"] = notes_new

        save_local_tasks()
        refresh_task_listbox()

        # Send to ESP
        payload = {
            "cmd": "EDIT_TASK",
            "id": esp_id,
            "index": idx,
            "title": title_new,
            "month": month_new,
            "day": day_new,
            "time": time_str,
            "priority": 1 if eod_local.get() else 0,
            "status": status_new,
        }
        if notes_new:
            payload["notes"] = notes_new

        send_json(payload)
        win.destroy()

    ttk.Button(win, text="Save Changes", command=save_changes).pack(pady=10)


def list_tasks():
    send_json({"cmd": "LIST_TASKS"})


def clear_all():
    if messagebox.askyesno("Confirm", "Clear all tasks here and on ESP?"):
        send_json({"cmd": "CLEAR_ALL"})
        tasks_cache.clear()
        save_local_tasks()
        refresh_task_listbox()


def refresh_task_listbox():
    task_listbox.delete(0, tk.END)
    for t in tasks_cache:
        title = t.get("title", "Untitled")
        status = t.get("status", "") or ""
        month = t.get("month", "") or ""
        day = t.get("day", 0) or 0

        # Normalise weird placeholder values
        if status in ("None", "NULL"):
            status = ""

        try:
            day_int = int(day)
        except Exception:
            day_int = 0

        if month in ("None", "", None) or day_int == 0:
            date_str = ""
        else:
            date_str = f"{month} {day_int}"

        line = title
        if status:
            line += f" ({status})"
        if date_str:
            line += f" [{date_str}]"

        task_listbox.insert(tk.END, line)

    if color_preview:
        for i, t in enumerate(tasks_cache):
            status = t.get("status", "") or ""
            if status in ("None", "NULL"):
                status = ""
            if status:
                task_listbox.itemconfig(i, fg=STATUS_COLORS.get(status, "#FFFFFF"))
            else:
                task_listbox.itemconfig(i, fg="#FFFFFF")


def toggle_colors():
    global color_preview
    color_preview = show_colors_var.get()
    refresh_task_listbox()


def move_task(delta):
    sel = task_listbox.curselection()
    if not sel:
        return
    idx = sel[0]
    new_idx = idx + delta
    if new_idx < 0 or new_idx >= len(tasks_cache):
        return

    src_task = tasks_cache[idx]
    dst_task = tasks_cache[new_idx]

    # Reorder locally
    task = tasks_cache.pop(idx)
    tasks_cache.insert(new_idx, task)
    save_local_tasks()
    refresh_task_listbox()
    task_listbox.selection_clear(0, tk.END)
    task_listbox.selection_set(new_idx)

    # Inform ESP (supports both index and stable id if firmware uses it)
    send_json({
        "cmd": "MOVE_TASK",
        "src": idx,
        "dst": new_idx,
        "src_id": src_task.get("id", idx),
        "dst_id": dst_task.get("id", new_idx),
    })

    # Ask ESP for a fresh list so both sides stay aligned
    list_tasks()


def toggle_screen():
    global screen_on
    send_json({"cmd": "SCREEN", "state": "OFF" if screen_on else "ON"})
    screen_on = not screen_on
    screen_btn.config(text="Turn Screen Off" if screen_on else "Turn Screen On")


# ---------------- Import / Export ----------------
def export_tasks():
    """Write current local tasks_cache to a JSON file on disk."""
    if not tasks_cache:
        messagebox.showinfo("Export", "No tasks to export.")
        return

    filename = filedialog.asksaveasfilename(
        title="Export Tasks JSON",
        defaultextension=".json",
        filetypes=[("JSON Files", "*.json"), ("All Files", "*.*")],
        initialfile="tasks_export.json",
        initialdir=os.getcwd(),
    )
    if not filename:
        return

    try:
        with open(filename, "w") as f:
            json.dump({"tasks": tasks_cache}, f, indent=2)
        messagebox.showinfo("Export", f"Exported {len(tasks_cache)} tasks to:\n{filename}")
    except Exception as e:
        messagebox.showerror("Export Error", f"Failed to export: {e}")


def import_tasks():
    """Open a JSON file and merge into current tasks (dedupe), including notes."""
    filename = filedialog.askopenfilename(
        title="Import Tasks JSON",
        filetypes=[("JSON Files", "*.json"), ("All Files", "*.*")],
        initialdir=os.getcwd(),
    )
    if not filename:
        return

    try:
        with open(filename, "r") as f:
            data = json.load(f)
        incoming = data.get("tasks", [])
    except Exception as e:
        messagebox.showerror("Error", f"Failed to read: {e}")
        return

    added = 0
    for inc in incoming:
        # normalise weird placeholders
        if inc.get("status") in ("None", "NULL"):
            inc["status"] = ""
        if inc.get("notes") in ("None", "NULL"):
            inc["notes"] = ""

        if not is_duplicate_local(inc, tasks_cache):
            tasks_cache.append(inc)

            payload = {
                "cmd": "ADD_TASK",
                "title": inc.get("title", "Untitled"),
                "month": inc.get("month", "None"),
                "day": inc.get("day", 0),
                "priority": inc.get("priority", 0),
            }
            if inc.get("time"):
                payload["time"] = inc["time"]
            if inc.get("status"):
                payload["status"] = inc["status"]
            notes_val = inc.get("notes", "")
            if notes_val:
                # Respect max length on wire as well
                payload["notes"] = str(notes_val)[:NOTES_MAX_LEN]

            send_json(payload)
            added += 1

    save_local_tasks()
    refresh_task_listbox()
    messagebox.showinfo("Import", f"Imported {added} new tasks (duplicates skipped).")


def is_duplicate_local(t, arr):
    """Return True if task already exists in arr (title+month+day+time+priority)."""
    for x in arr:
        if (
            x.get("title", "") == t.get("title", "")
            and (x.get("month", "None") or "None") == (t.get("month", "None") or "None")
            and int(x.get("day", 0) or 0) == int(t.get("day", 0) or 0)
            and (x.get("time", "") or "") == (t.get("time", "") or "")
            and int(x.get("priority", 0) or 0) == int(t.get("priority", 0) or 0)
        ):
            return True
    return False


# ---------------- GUI ----------------
root = tk.Tk()
root.title("ESP32 Task Editor v16")
root.geometry("1080x720")

main = ttk.Frame(root, padding=10)
main.pack(fill="both", expand=True)

# Serial row
ttk.Label(main, text="Serial Port:").grid(row=0, column=0, sticky="w")
ports = [p.device for p in serial.tools.list_ports.comports()]
port_combo = ttk.Combobox(main, values=ports, width=15)
port_combo.grid(row=0, column=1, sticky="w", padx=5)
ttk.Button(main, text="Connect", command=connect_serial).grid(row=0, column=2, padx=5)

# Calendar (left)
today = datetime.today()
cal = Calendar(
    main, selectmode="day", year=today.year, month=today.month, day=today.day
)
cal.grid(row=1, column=0, columnspan=3, pady=10, sticky="w")

# Title
ttk.Label(main, text="Task Title:").grid(row=2, column=0, sticky="w")
title_entry = ttk.Entry(main, width=50)
title_entry.grid(row=2, column=1, columnspan=2, sticky="w", pady=5)

# Notes (new, optional)
ttk.Label(main, text=f"Notes (optional, max {NOTES_MAX_LEN} chars):").grid(
    row=3, column=0, sticky="nw"
)
notes_text = tk.Text(main, width=50, height=4, wrap="word")
notes_text.grid(row=3, column=1, columnspan=2, sticky="w", pady=5)

# Time frame
time_frame = ttk.LabelFrame(main, text="Optional Time")
time_frame.grid(row=4, column=0, columnspan=3, sticky="w", pady=5)
ttk.Label(time_frame, text="Hour:").grid(row=0, column=0, padx=3)
hour_entry = ttk.Entry(time_frame, width=5)
hour_entry.grid(row=0, column=1)
ttk.Label(time_frame, text="Min:").grid(row=0, column=2, padx=3)
min_entry = ttk.Entry(time_frame, width=5)
min_entry.grid(row=0, column=3)
ampm_var = tk.StringVar(value="PM")
ttk.Radiobutton(time_frame, text="AM", variable=ampm_var, value="AM").grid(
    row=0, column=4, padx=4
)
ttk.Radiobutton(time_frame, text="PM", variable=ampm_var, value="PM").grid(
    row=0, column=5, padx=4
)

# EOD + No Date
options_frame = ttk.Frame(main)
options_frame.grid(row=5, column=0, columnspan=3, sticky="w", pady=5)

eod_var = tk.BooleanVar(value=False)
no_date_var = tk.BooleanVar(value=False)

def eod_clicked():
    if eod_var.get():
        no_date_var.set(False)

def nodate_clicked():
    if no_date_var.get():
        eod_var.set(False)

ttk.Checkbutton(
    options_frame,
    text="End of Day (priority)",
    variable=eod_var,
    command=eod_clicked,
).grid(row=0, column=0, padx=4)
ttk.Checkbutton(
    options_frame, text="No Date", variable=no_date_var, command=nodate_clicked
).grid(row=0, column=1, padx=4)

# Buttons
btn_frame = ttk.Frame(main)
btn_frame.grid(row=6, column=0, columnspan=3, sticky="w", pady=10)
ttk.Button(btn_frame, text="Add Task", command=add_task).grid(row=0, column=0, padx=4)
ttk.Button(btn_frame, text="Edit Task", command=edit_task).grid(row=0, column=1, padx=4)
ttk.Button(btn_frame, text="Remove Task", command=remove_task).grid(
    row=0, column=2, padx=4
)
ttk.Button(btn_frame, text="List / Refresh", command=list_tasks).grid(
    row=0, column=3, padx=4
)
ttk.Button(btn_frame, text="Clear All", command=clear_all).grid(
    row=0, column=4, padx=4
)
ttk.Button(btn_frame, text="Import JSON", command=import_tasks).grid(
    row=0, column=5, padx=4
)
ttk.Button(btn_frame, text="Export JSON", command=export_tasks).grid(
    row=0, column=6, padx=4
)
ttk.Button(btn_frame, text="Sync Time", command=sync_time).grid(
    row=0, column=7, padx=4
)

# Right panel
right = ttk.Frame(main)
right.grid(row=1, column=3, rowspan=6, padx=10, sticky="ns")
screen_btn = ttk.Button(right, text="Turn Screen Off", command=toggle_screen)
screen_btn.pack(pady=3)

show_colors_var = tk.BooleanVar(value=True)
ttk.Checkbutton(
    right, text="Show Status Colors", variable=show_colors_var, command=toggle_colors
).pack(pady=3, anchor="w")

ttk.Label(right, text="Task List (ESP32)").pack(anchor="w", pady=(6, 2))

# Dark listbox to avoid white-on-white
task_listbox = tk.Listbox(
    right,
    width=40,
    height=18,
    bg="#1e1e1e",
    fg="#00ff00",
    selectbackground="#335533",
    selectforeground="white",
    highlightbackground="#335533",
    relief="flat",
    font=("Consolas", 11, "bold"),
)
task_listbox.pack(padx=2, pady=4)

move_frame = ttk.Frame(right)
move_frame.pack(pady=5)
ttk.Button(move_frame, text="↑ Move Up", command=lambda: move_task(-1)).grid(
    row=0, column=0, padx=3
)
ttk.Button(move_frame, text="↓ Move Down", command=lambda: move_task(1)).grid(
    row=0, column=1, padx=3
)

# Log window
log = tk.Text(
    main, height=10, width=140, bg="#0f0f0f", fg="#00ff00", insertbackground="white"
)
log.grid(row=7, column=0, columnspan=4, pady=10)


def on_close():
    global running
    running = False
    if ser:
        ser.close()
    root.destroy()


root.protocol("WM_DELETE_WINDOW", on_close)

# init + threads
load_local_tasks()
refresh_task_listbox()

# background time sync
threading.Thread(target=time_sync_loop, daemon=True).start()
# periodic task list refresh from ESP
threading.Thread(target=periodic_refresh_loop, daemon=True).start()

root.mainloop()
