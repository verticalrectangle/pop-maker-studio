import tkinter as tk
from tkinter import filedialog, messagebox
import subprocess
import sys

def run(path):
    subprocess.run([sys.executable, "../main.py", path])

def select_file():
    path = filedialog.askopenfilename(
        filetypes=[("Audio Files", "*.mp3 *.wav *.m4a *.flac")]
    )
    if path:
        try:
            run(path)
            messagebox.showinfo("Done", "Processing complete!")
        except Exception as e:
            messagebox.showerror("Error", str(e))

def main():
    root = tk.Tk()
    root.title("Song2Subs")
    root.geometry("300x150")

    btn = tk.Button(root, text="Select Audio File", command=select_file)
    btn.pack(expand=True)

    root.mainloop()

if __name__ == "__main__":
    main()
