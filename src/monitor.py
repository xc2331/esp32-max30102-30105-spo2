import tkinter as tk
from tkinter import font, messagebox
import serial
import serial.tools.list_ports
import threading
import queue
import time
import sys

# ================= 配置区域 =================
BAUD_RATE = 115200 
COM_PORT = None     
# ===========================================

class Spo2Monitor:
    def __init__(self, root):
        self.root = root
        self.root.title("血氧监测仪 (高稳版)")
        self.root.geometry("500x450")
        self.root.configure(bg="#f0f0f0")
        self.root.protocol("WM_DELETE_WINDOW", self.on_closing)

        self.data_queue = queue.Queue()
        self.serial_port = None
        self.is_running = False
        
        # 【优化】去重变量
        self.last_msg_content = ""           # 记录上一条 MSG 的内容
        
        # 线程锁，防止多线程同时操作 UI 变量（虽然主要操作在主线程，但加锁更安全）
        self.ui_lock = threading.Lock()

        self.setup_ui()
        self.connect_serial()

        if self.serial_port:
            self.is_running = True
            self.thread = threading.Thread(target=self.read_serial_loop, daemon=True)
            self.thread.start()
            self.update_ui_loop()
        else:
            self.show_error_on_screen("无法连接串口\n请检查设备连接及端口设置")

    def setup_ui(self):
        tk.Label(self.root, text="实时血氧饱和度监测", font=("Arial", 18, "bold"), bg="#f0f0f0").pack(pady=15)
        
        self.font_value = font.Font(family="Helvetica", size=80, weight="bold")
        self.lbl_value = tk.Label(self.root, text="--", font=self.font_value, bg="#f0f0f0", fg="#ccc")
        self.lbl_value.pack(pady=5)
        
        tk.Label(self.root, text="% SpO2", font=("Arial", 14), bg="#f0f0f0").pack()
        
        self.font_msg = font.Font(family="Arial", size=14)
        self.label_msg = tk.Label(self.root, text="正在初始化...", font=self.font_msg, bg="#f0f0f0", fg="#555", wraplength=450, justify="center")
        self.label_msg.pack(pady=20)
        
        tk.Label(self.root, text="串口监视 (重复报错自动过滤)", font=("Arial", 10), bg="#f0f0f0").pack()
        self.log_text = tk.Text(self.root, height=8, width=60, bg="#2b2b2b", fg="#00ff00", font=("Consolas", 10))
        self.log_text.pack(pady=5, padx=10, fill=tk.BOTH, expand=True)
        self.log_text.config(state='disabled')

    def log(self, message, msg_type=None):
        """
        【重构版】日志函数
        策略：如果是重复的 MSG，直接跳过不写入，避免频繁 delete/insert 导致崩溃。
        这样界面上永远保留该错误的第一次出现（或上一次不同错误），且绝不会刷屏。
        """
        # 如果是重复的 MSG，直接返回，不进行任何 UI 操作（最稳定）
        if msg_type == 'MSG' and message == self.last_msg_content:
            return 

        timestamp = time.strftime("%H:%M:%S")
        full_msg = f"[{timestamp}] {message}"
        
        try:
            with self.ui_lock:
                self.log_text.config(state='normal')
                
                if msg_type == 'DATA':
                    # 收到正常数据，重置 MSG 记录
                    self.last_msg_content = ""
                    self.log_text.insert(tk.END, full_msg + "\n")
                    
                elif msg_type == 'MSG':
                    # 收到新类型的 MSG（与上一条不同），写入并更新记录
                    self.last_msg_content = message
                    self.log_text.insert(tk.END, full_msg + "\n")
                    
                else:
                    # 系统消息
                    self.last_msg_content = ""
                    self.log_text.insert(tk.END, full_msg + "\n")

                # 自动滚动
                self.log_text.see(tk.END)
                self.log_text.config(state='disabled')
                
        except Exception as e:
            # 静默失败，防止崩溃
            # print(f"Log error ignored: {e}") 
            pass

    def connect_serial(self):
        self.log("开始搜索串口...", None)
        if COM_PORT:
            try:
                self.serial_port = serial.Serial(COM_PORT, BAUD_RATE, timeout=1)
                self.log(f"✅ 已连接：{COM_PORT}", None)
                self.label_msg.config(text="设备已连接", fg="green")
                return
            except Exception as e:
                self.log(f"❌ 连接失败：{str(e)}", None)
                self.serial_port = None
                return

        ports = list(serial.tools.list_ports.comports())
        for port in ports:
            keywords = ["Arduino", "CH340", "CP2102", "Silicon Labs", "USB Serial"]
            if any(k in port.description for k in keywords):
                try:
                    self.serial_port = serial.Serial(port.device, BAUD_RATE, timeout=1)
                    self.log(f"✅ 自动匹配：{port.device}", None)
                    self.label_msg.config(text=f"已连接：{port.device}", fg="green")
                    return
                except:
                    continue
        
        self.log("❌ 未找到设备", None)
        self.serial_port = None

    def show_error_on_screen(self, msg):
        self.lbl_value.config(text="--", fg="red")
        self.label_msg.config(text=msg, fg="red", font=("Arial", 12))

    def read_serial_loop(self):
        while self.is_running:
            if self.serial_port and self.serial_port.is_open:
                try:
                    if self.serial_port.in_waiting > 0:
                        line = self.serial_port.readline().decode('utf-8', errors='ignore').strip()
                        if line:
                            self.data_queue.put(line)
                except Exception as e:
                    if self.is_running:
                        # 串口断开处理
                        self.data_queue.put(f"SYS:串口连接丢失 - {str(e)}")
                        self.is_running = False
                    break
            else:
                time.sleep(0.5)

    def update_ui_loop(self):
        if not self.is_running:
            return

        try:
            while not self.data_queue.empty():
                line = self.data_queue.get_nowait()
                
                if line.startswith("SYS:"):
                    # 系统消息
                    self.log(line[4:], msg_type=None)
                    continue

                if line.startswith("DATA:"):
                    try:
                        val_str = line.split(":")[1]
                        val = int(val_str)
                        self.lbl_value.config(text=str(val), fg="#007bff")
                        self.label_msg.config(text="测量正常", fg="green")
                        self.log(line, msg_type='DATA')
                    except Exception:
                        self.log(f"数据解析错误：{line}", msg_type='MSG')
                        
                elif line.startswith("MSG:"):
                    reason = line.split(":", 1)[1]
                    self.lbl_value.config(text="--", fg="#ccc")
                    self.label_msg.config(text=f"原因：{reason}", fg="#d9534f")
                    self.log(line, msg_type='MSG')
                    
        except Exception:
            pass
        
        if self.is_running:
            self.root.after(100, self.update_ui_loop)

    def on_closing(self):
        self.is_running = False
        if self.serial_port:
            try:
                self.serial_port.close()
            except:
                pass
        try:
            self.root.destroy()
        except:
            pass
        sys.exit(0)

if __name__ == "__main__":
    try:
        root = tk.Tk()
        # 设置高 DPI 支持（可选，防止字体模糊）
        try:
            from ctypes import windll
            windll.shcore.SetProcessDpiAwareness(1)
        except:
            pass
            
        app = Spo2Monitor(root)
        root.mainloop()
    except KeyboardInterrupt:
        sys.exit(0)
    except Exception as e:
        import traceback
        print(f"Startup Error: {e}")
        traceback.print_exc()