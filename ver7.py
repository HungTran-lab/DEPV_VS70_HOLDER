import os
import sys
import csv
import re
import serial
import serial.tools.list_ports
from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QPushButton, QComboBox, QLineEdit,
    QDesktopWidget, QMessageBox, QTableWidgetItem
)
from PyQt5.uic import loadUi
from PyQt5.QtGui import QPixmap, QImage, QPainter, QColor, QFont, QIcon, QTextCursor
from PyQt5.QtCore import QTimer, QDateTime, Qt, pyqtSignal
from PyQt5.QtPrintSupport import QPrinter
from datetime import datetime
import qrcode
from PIL import Image

# -------------------------
# PyInstaller helpers
# -------------------------
def resource_path(rel_path: str) -> str:
    """Get absolute path to resource, works for dev and for PyInstaller --onefile."""
    base_path = getattr(sys, "_MEIPASS", os.path.abspath(os.path.dirname(__file__)))
    return os.path.join(base_path, rel_path)

def app_dir() -> str:
    """Writable base directory (beside the .exe when frozen, else beside this .py)."""
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    return os.path.abspath(os.path.dirname(__file__))


def pil_to_qimage(pil_img):
    """Convert PIL.Image to QImage without relying on PIL.ImageQt (works even if ImageQt is unavailable)."""
    img = pil_img.convert("RGBA")
    w, h = img.size
    try:
        fmt = getattr(QImage, "Format_RGBA8888")
        data = img.tobytes("raw", "RGBA")
        qimg = QImage(data, w, h, fmt)
    except Exception:
        # Fallback for older Qt: QImage.Format_ARGB32 uses BGRA byte order on little-endian platforms
        data = img.tobytes("raw", "BGRA")
        qimg = QImage(data, w, h, QImage.Format_ARGB32)
    return qimg.copy()

# =================== COLOR CONSTANTS ===================
COLOR_BLUE = "background-color: rgb(0, 0, 255); border-radius: 20px;"
COLOR_RED = "background-color: red; border-radius: 20px;"
COLOR_OK = "color: green; background-color: lightblue;"
COLOR_NG = "color: red; background-color: lightblue;"
COLOR_WAIT = "color: orange; background-color: lightyellow;"


class MyWindow(QMainWindow):
    date_signal = pyqtSignal(str, str, str)

    def __init__(self):
        super().__init__()

        # Paths (writable)
        self._app_dir = app_dir()
        self._config_path = os.path.join(self._app_dir, "config.csv")
        self._counter_path = os.path.join(self._app_dir, "data.csv")
        self._data_dir = os.path.join(self._app_dir, "data")

        self._load_ui()
        self.setWindowTitle("FT Assy Charger Base")
        # Window icon (from bundled DEPV.ico)
        icon_file = resource_path("DEPV.ico")
        if os.path.exists(icon_file):
            self.setWindowIcon(QIcon(icon_file))
        self.setFixedSize(1380, 670)
        self.center_window()

        # init Table
        self.data_table.setColumnCount(4)
        self.data_table.setHorizontalHeaderLabels(["No.", "Time", "ADC Value", "Result"])

        # Khởi tạo biến
        self.serial_connection = None
        self._saved_com_port = ""  # last COM saved in config.csv
        self._rx_buffer = bytearray()  # serial RX buffer (non-blocking)
        self.last_state = "NONE"
        self.qr_image = None  # tránh lỗi khi in trước lúc tạo QR
        self.counter_date = None  # mốc ngày hiện hành của counter

        # Load config + counter (có auto-reset theo ngày)
        self.load_config()
        self.load_counter()

        self.ok_count.setText(f"{self.ok_count_value:04d}")
        self.ng_count.setText(f"{self.ng_count_value:04d}")
        self.total_count.setText(f"{self.total_count_value:04d}")

        # timer update time & COM read
        timer = QTimer(self)
        timer.timeout.connect(self.update_time)
        timer.start(1000)

        self.serial_timer = QTimer(self)
        self.serial_timer.timeout.connect(self.read_from_com)
        self.serial_timer.start(100)

        # signals
        self.connect_button.clicked.connect(self.connect_com)
        self.comboBox_com_ports.currentIndexChanged.connect(self.update_serial_port)
        self.make_qr.clicked.connect(self.make_qr_code1)
        self.print_qr.clicked.connect(self.print_qr_code)
        # self.actionManual.triggered.connect(self.show_manual_message)
        self.actionVer.triggered.connect(self.show_about_message)
        self.actionInfor.triggered.connect(self.show_infor_message)

        # ---- đồng bộ show_model với comboBox (model) ----
        self.show_model.setReadOnly(True)  # chỉ hiển thị, không cho sửa
        self.comboBox.currentTextChanged.connect(self.on_combo_model_changed)
        self.on_combo_model_changed(self.comboBox.currentText())  # set giá trị ban đầu
        # ---- gửi model xuống COM khi bấm nút ----
        self.set_model.clicked.connect(self.handle_set_model_clicked)
        # nút không tự lặp khi giữ
        self.set_model.setAutoRepeat(False)

        # ports
        self.populate_com_ports()

        # init state
        self.reset_sensors()
        self.display.setPlainText("")
        self.status.setReadOnly(True)
        self.qr_print.setReadOnly(True)
        self.dept.setReadOnly(True)
        self.company.setReadOnly(True)


    def _load_ui(self):
        """Load .ui file in both dev and PyInstaller --onefile modes."""
        candidates = ["gui_2.ui", "gui2.ui", "gui_2.ui.ui"]
        for ui_name in candidates:
            ui_path = resource_path(ui_name)
            if os.path.exists(ui_path):
                loadUi(ui_path, self)
                return
        # Friendly error
        QMessageBox.critical(
            self,
            "UI not found",
            "Không tìm thấy file UI (gui_2.ui / gui2.ui).\n"
            "Hãy đặt file .ui cùng thư mục với file .py hoặc cùng thư mục với file .exe."
        )
        raise FileNotFoundError("UI file not found: " + ", ".join(candidates))

    # ================== TIME UPDATE ==================
    def update_time(self):
        current_datetime = QDateTime.currentDateTime()
        time_str = current_datetime.toString("HH:mm:ss")
        self.label_time.setText(time_str)
        date_str = current_datetime.toString("dd-MM-yyyy")
        self.label_date.setText(date_str)

        # Cập nhật năm
        year = current_datetime.toString("yyyy")
        year_mapping = {
            "2025": "Y",
            "2026": "L",
            "2027": "P",
        }
        self.year_display = year_mapping.get(year, year)
        self.nam.setText(self.year_display)

        # Cập nhật tháng
        month = current_datetime.toString("M")
        month_mapping = {
            "10": "A",
            "11": "B",
            "12": "C"
        }
        self.month_display = month_mapping.get(month, month)
        self.thang.setText(self.month_display)

        # Cập nhật ngày
        day = current_datetime.toString("d")
        day_mapping = {
            "1": "1",
            "2": "2",
            "3": "3",
            "4": "4",
            "5": "5",
            "6": "6",
            "7": "7",
            "8": "8",
            "9": "9",
            "10": "A",
            "11": "B",
            "12": "C",
            "13": "D",
            "14": "E",
            "15": "F",
            "16": "G",
            "17": "H",
            "18": "J",
            "19": "K",
            "20": "L",
            "21": "M",
            "22": "N",
            "23": "P",
            "24": "R",
            "25": "S",
            "26": "T",
            "27": "V",
            "28": "W",
            "29": "X",
            "30": "Y",
            "31": "Z",
        }
        self.day_display = day_mapping.get(day, day)
        self.ngay.setText(self.day_display)

        # Kiểm tra sang ngày mới để auto reset counter
        self._daily_reset_if_needed(reason="clock")

    # ================== COM PORT ==================
    def populate_com_ports(self):
        ports = serial.tools.list_ports.comports()
        self.comboBox_com_ports.clear()
        for port in ports:
            self.comboBox_com_ports.addItem(port.device)

        # Restore last saved COM (nếu còn tồn tại)
        if getattr(self, "_saved_com_port", ""):
            idx = self.comboBox_com_ports.findText(self._saved_com_port)
            if idx != -1:
                self.comboBox_com_ports.setCurrentIndex(idx)

    def connect_com(self):
        selected_port = self.comboBox_com_ports.currentText()
        if selected_port:
            if self.serial_connection and self.serial_connection.is_open:
                self.serial_connection.close()
                self.comboBox_com_ports.setEnabled(True)
                self.status.setText(f"Disconnected from {selected_port}")
                self.status.setStyleSheet("color: orange;")
                self.set_model.setEnabled(False)   # <--- thêm
            else:
                try:
                    self.serial_connection = serial.Serial(selected_port, 115200, timeout=0)
                    self.save_config_value("COM Port", selected_port)
                    self.comboBox_com_ports.setEnabled(False)
                    self.status.setText(f"Connected to {selected_port} - 115200")
                    self.status.setStyleSheet("color: green;")
                    self.set_model.setEnabled(True)    # <--- thêm
                except serial.SerialException as e:
                    self.status.setText(f"Failed to connect: {e}")
                    self.status.setStyleSheet("color: red;")
                    self.set_model.setEnabled(False)   # <--- thêm

    def update_serial_port(self):
        # Lưu COM mỗi khi user đổi selection
        selected_port = self.comboBox_com_ports.currentText().strip()
        if selected_port:
            self.save_config_value("COM Port", selected_port)

        if self.serial_connection and self.serial_connection.is_open:
            self.serial_connection.close()
            self.status.setText("Disconnected from old port")
            self.status.setStyleSheet("color: orange;")

    def reconnect_com(self):
        if self.serial_connection:
            self.serial_connection.close()
        selected_port = self.comboBox_com_ports.currentText()
        if selected_port:
            try:
                self.serial_connection = serial.Serial(selected_port, 115200, timeout=0)
                self.comboBox_com_ports.setEnabled(False)
                self.status.setText(f"Reconnected to {selected_port}")
                self.status.setStyleSheet("color: green;")
                self.set_model.setEnabled(True)        # <--- thêm
            except serial.SerialException:
                self.comboBox_com_ports.setEnabled(True)
                self.status.setText("Failed to reconnect")
                self.status.setStyleSheet("color: red;")
                self.set_model.setEnabled(False)       # <--- thêm

    # ================== SERIAL READ ==================
    def read_from_com(self):
        # Non-blocking serial read: đọc hết dữ liệu đang chờ, tự tách dòng theo "\n".
        if not self.serial_connection or not self.serial_connection.is_open:
            return
        try:
            n = self.serial_connection.in_waiting
            if not n:
                return

            data = self.serial_connection.read(n)
            if not data:
                return

            self._rx_buffer.extend(data)

            # Tách từng dòng theo "\n" (tránh block UI)
            while True:
                idx = self._rx_buffer.find(b'\n')
                if idx == -1:
                    break
                line_bytes = self._rx_buffer[:idx]
                del self._rx_buffer[: idx + 1]

                line = line_bytes.decode('utf-8', errors='ignore').strip()
                if line:
                    self.append_limited_log(line)
                    self.process_line(line)

        except serial.SerialException:
            self.status.setText("Lỗi cổng COM")
            self.status.setStyleSheet("color: red;")
            self.reconnect_com()


    # ================== PARSER & DAILY-RESET HELPERS ==================
    def _extract_adc_payload(self, line: str):
        """Tìm phần sau data= / Data: (case-insensitive) và trả về chuỗi '0,1,0,1,0' hoặc None."""
        low = line.lower()
        for token in ("data=", "data:"):
            i = low.find(token)
            if i != -1:
                return line[i + len(token):].strip()
        return None

    def _apply_sensor_colors(self, values):
        """
        Ánh xạ 5 giá trị sang 5 QLabel sc_1..sc_5:
        - '1' -> đỏ (NG)
        - khác -> xanh (mặc định)
        """
        for i in range(1, 6):  # 1..5
            label = getattr(self, f"sc_{i}")
            if i <= len(values) and values[i-1].strip() == "1":
                label.setStyleSheet(COLOR_RED)
            else:
                label.setStyleSheet(COLOR_BLUE)

    def _today_str(self):
        return datetime.now().strftime("%Y-%m-%d")

    def _get_data_csv_date(self):
        """Lấy ngày (YYYY-MM-DD) dựa vào mtime của data.csv; nếu không có file thì trả về hôm nay."""
        try:
            if os.path.exists(self._counter_path):
                ts = os.path.getmtime(self._counter_path)
                return datetime.fromtimestamp(ts).strftime("%Y-%m-%d")
        except Exception:
            pass
        return self._today_str()

    def _daily_reset_if_needed(self, reason="timer"):
        """Nếu đã sang ngày mới so với self.counter_date thì reset counter về 0 và lưu lại."""
        today = self._today_str()
        if getattr(self, "counter_date", None) != today:
            # Reset biến
            self.ok_count_value = 0
            self.ng_count_value = 0
            self.total_count_value = 0

            # Cập nhật UI
            self.ok_count.setText("0000")
            self.ng_count.setText("0000")
            self.total_count.setText("0000")

            # Lưu lại và cập nhật mốc ngày
            self.save_counter()
            self.counter_date = today

            # Log nhẹ để biết đã reset
            self.append_limited_log(f"[Auto-reset counters for new day ({reason})]")

    # ================== PROCESS LINE ==================
    def process_line(self, line):
        s = line.strip()
        up = s.upper()

        # START -> reset 5 QLabel
        if up.startswith("START"):
            self.reset_sensors()
            self.value.setText("Test..")
            self.value.setStyleSheet(COLOR_WAIT)
            return

        # Waiting
        if up.startswith("WAITING"):
            self.value.setText("Wait")
            self.value.setStyleSheet(COLOR_WAIT)
            self.reset_sensors()
            return

        # Lấy payload ADC nếu có (hỗ trợ 'data=' hoặc 'Data:')
        adc_str = self._extract_adc_payload(s)

        # OK
        if up.startswith("OK"):
            self.value.setText("OK")
            self.value.setStyleSheet(COLOR_OK)
            self.reset_sensors()

            # hiển thị ADC nếu có
            if adc_str is not None:
                self.value_adc.setText(adc_str)

            # cập nhật bộ đếm
            self.ok_count_value += 1
            self.ok_count.setText(f"{self.ok_count_value:04d}")
            self.total_count_value = self.ok_count_value + self.ng_count_value
            self.total_count.setText(f"{self.total_count_value:04d}")

            # QR & lưu
            self.make_qr_code1()
            self.print_qr_code()
            self.save_qlineedit_to_csv()
            self.save_counter()
            return

        # NG
        if up.startswith("NG"):
            self.value.setText("NG")
            self.value.setStyleSheet(COLOR_NG)

            # reset trước rồi tô lại theo payload
            self.reset_sensors()

            if adc_str is not None:
                self.value_adc.setText(adc_str)
                values = [v.strip() for v in adc_str.split(",") if v.strip() != ""]
                # tô màu theo 5 giá trị (1 -> đỏ)
                self._apply_sensor_colors(values)

            # cập nhật bộ đếm
            self.ng_count_value += 1
            self.ng_count.setText(f"{self.ng_count_value:04d}")
            self.total_count_value = self.ok_count_value + self.ng_count_value
            self.total_count.setText(f"{self.total_count_value:04d}")

            self.save_qlineedit_to_csv()
            self.save_counter()
            return

        # (Tuỳ chọn) các dòng khác: không làm gì

    def append_limited_log(self, text_line, max_lines=20):
        cursor = self.display.textCursor()
        cursor.movePosition(QTextCursor.End)
        cursor.insertText(text_line + '\n')
        lines = self.display.toPlainText().splitlines()
        if len(lines) > max_lines:
            self.display.clear()
            self.display.appendPlainText('\n'.join(lines[-max_lines:]))
        self.display.moveCursor(QTextCursor.End)

    # ================== QR CODE ==================
    

    def make_qr_code1(self):
        # ==== Lấy dữ liệu tạo QR ====
        year  = self.year_display
        month = self.month_display
        day   = self.day_display

        data3 = self.dept.text()
        data4 = self.company.text()   # (đang không dùng trong chuỗi, nhưng giữ lại nếu cần)
        additional_text = self.comboBox.currentText()
        counter_str = f"{self.ok_count_value:04d}"

        # Chuỗi QR giữ nguyên định dạng cũ
        qr_data = "".join(["18", additional_text, data3, year[-1], month, day, counter_str])
        self.qr_print.setText(qr_data)

        # ==== Tham số bố cục có thể chỉnh nhanh ====
        QR_SIDE      = 300           # Kích thước QR vuông (px) — có thể đổi 236/280/320...
        PADDING      = 12            # Lề xung quanh
        TEXT_AREA_H  = 72            # Vùng dành cho chữ (2 dòng)
        CANVAS_W     = QR_SIDE + 2*PADDING
        CANVAS_H     = QR_SIDE + 2*PADDING + TEXT_AREA_H

        FONT_MAIN_SZ = 20            # size chữ dòng 1 (additional_text)
        FONT_SUB_SZ  = 14            # size chữ dòng 2 (qr_data)

        # ==== Tạo ảnh QR (PIL) ====
        qr = qrcode.QRCode(version=1, box_size=12, border=2)
        qr.add_data(qr_data)
        qr.make(fit=True)
        qr_pil_image = qr.make_image(fill_color="black", back_color="white")
        qr_pil_image = qr_pil_image.resize((QR_SIDE, QR_SIDE), resample=Image.NEAREST)  # Giữ cạnh sắc nét cho QR
        # Chuyển PIL -> QImage nhanh (không cần PIL.ImageQt)
        qr_qimage = pil_to_qimage(qr_pil_image)
        # ==== Vẽ canvas tổng hợp ====
        canvas = QPixmap(CANVAS_W, CANVAS_H)
        canvas.fill(Qt.white)
        painter = QPainter(canvas)

        # Vẽ QR (căn giữa theo chiều ngang)
        qr_x = (CANVAS_W - QR_SIDE) // 2
        qr_y = PADDING
        painter.drawImage(qr_x, qr_y, qr_qimage)

        # Vẽ 2 dòng chữ bên dưới
        painter.setPen(Qt.black)

        # Dòng 1: additional_text
        font1 = QFont("SamsungSharpSans-Bold", FONT_MAIN_SZ)
        painter.setFont(font1)
        fm1 = painter.fontMetrics()
        text1 = fm1.elidedText(additional_text, Qt.ElideRight, CANVAS_W - 2*PADDING)
        text1_w = fm1.horizontalAdvance(text1)
        text1_x = (CANVAS_W - text1_w) // 2
        text1_baseline_y = qr_y + QR_SIDE + PADDING + fm1.ascent()
        painter.drawText(text1_x, text1_baseline_y, text1)

        # # Dòng 2: qr_data (nhỏ hơn, nằm dưới dòng 1)
        # font2 = QFont("SamsungSharpSans-Bold", FONT_SUB_SZ)
        # painter.setFont(font2)
        # fm2 = painter.fontMetrics()
        # text2 = fm2.elidedText(qr_data, Qt.ElideRight, CANVAS_W - 2*PADDING)
        # text2_w = fm2.horizontalAdvance(text2)
        # text2_x = (CANVAS_W - text2_w) // 2
        # text2_baseline_y = text1_baseline_y + fm2.height() + 2  # khoảng cách 2px
        # painter.drawText(text2_x, text2_baseline_y, text2)

        painter.end()

        # ==== Lưu & hiển thị ====
        canvas.save(os.path.join(self._app_dir, "qr_code.png"))
        self.qr_image = canvas
        self.label_qr_code.setPixmap(
            canvas.scaled(self.label_qr_code.size(), Qt.KeepAspectRatio, Qt.SmoothTransformation)
        )

    
    def print_qr_code(self):
        # Nếu checkbox enable_print đang bật -> không in (đang "disable print")
        if self.enable_print.isChecked():
            return
        if self.qr_image:
            printer = QPrinter(QPrinter.HighResolution)
            painter = QPainter(printer)
            rect = painter.viewport()
            size = self.qr_image.size()
            size.scale(rect.size(), Qt.KeepAspectRatio)
            painter.setViewport(rect.x(), rect.y(), size.width(), size.height())
            painter.setWindow(self.qr_image.rect())
            painter.drawPixmap(0, 0, self.qr_image)
            painter.end()

    # ================== SAVE ==================
    def save_qlineedit_to_csv(self):
        read_adc = self.value_adc.text().strip()   # Giá trị ADC
        status   = self.value.text().strip()       # Trạng thái OK/NG

        # Lấy chuỗi QR đang hiển thị
        try:
            qr_str = self.qr_print.text().strip()
        except AttributeError:
            qr_str = self.qr_print.toPlainText().strip()

        os.makedirs(self._data_dir, exist_ok=True)
        save_path = os.path.join(self._data_dir, f"adc_data_{datetime.now().strftime('%Y-%m-%d')}.csv")

        header_new = ["No.", "Date", "Time", "ADC Value", "Status", "S/N"]


        required_header = [h.lower() for h in header_new]

        def _file_date_from_name(path: str) -> str:
            # data/adc_data_YYYY-MM-DD.csv
            m = re.search(r"adc_data_(\d{4}-\d{2}-\d{2})\.csv$", os.path.basename(path))
            return m.group(1) if m else datetime.now().strftime("%Y-%m-%d")

        # --- Nâng cấp header nếu file đã tồn tại nhưng thiếu/khác cột
        if os.path.exists(save_path):
            try:
                with open(save_path, 'r', encoding='utf-8', newline='') as rf:
                    reader = csv.reader(rf)
                    cur_header = next(reader, [])
                    cur_norm = [c.strip().lower() for c in cur_header]

                    # Chỉ nâng cấp khi header khác với chuẩn
                    if cur_norm != required_header:
                        old_rows = list(reader)
                    else:
                        old_rows = None

                if old_rows is not None:
                    file_date = _file_date_from_name(save_path)
                    tmp_path = save_path + '.tmp'
                    with open(tmp_path, 'w', encoding='utf-8', newline='') as wf:
                        writer = csv.writer(wf)
                        writer.writerow(header_new)

                        for row in old_rows:
                            if not row:
                                continue

                            # Các format phổ biến:
                            # - cũ (4 cột): No, Time, ADC Value, Result
                            # - mới thiếu S/N (5 cột): No, Date, Time, ADC Value, Status
                            if len(row) == 4:
                                new_row = [row[0], file_date, row[1], row[2], row[3], '']
                            elif len(row) == 5:
                                new_row = row + ['']
                            else:
                                # Không rõ format: pad/trim về 6 cột
                                new_row = (row + [''] * 6)[:6]

                            writer.writerow(new_row)

                    os.replace(tmp_path, save_path)

            except Exception:
                # Nếu nâng cấp thất bại, bỏ qua để vẫn có thể ghi bản ghi mới
                pass


        # --- Đếm số dòng để xác định No.
        file_exists = os.path.exists(save_path)
        row_count = 0
        if file_exists:
            try:
                with open(save_path, 'r', encoding='utf-8') as cf:
                    row_count = sum(1 for _ in cf) - 1  # trừ header
                    if row_count < 0:
                        row_count = 0
            except Exception:
                row_count = 0

        # --- Ghi dữ liệu
        try:
            write_header = (not file_exists) or (os.path.getsize(save_path) == 0)
            with open(save_path, 'a', newline='', encoding='utf-8') as f:
                writer = csv.writer(f)
                if write_header:
                    writer.writerow(header_new)
                current_date = datetime.now().strftime("%Y-%m-%d")
                current_time = datetime.now().strftime("%H:%M:%S")
                writer.writerow([row_count + 1, current_date, current_time, read_adc, status, qr_str])
        except Exception as e:
            QMessageBox.critical(self, "Lỗi", f"Không thể lưu dữ liệu: {str(e)}")

    # ================== RESET ==================
    def reset_sensors(self):
        for i in range(1, 6):  # 1..5
            label = getattr(self, f"sc_{i}")
            label.setStyleSheet(COLOR_BLUE)

    # ================== COUNTER + CONFIG ==================

    def save_config_value(self, key_name: str, value: str):
        """
        Update/append 1 key-value into config.csv (2 columns).
        Giữ lại các key khác (nếu có).
        """
        try:
            rows = []
            found = False
            if os.path.exists(self._config_path):
                with open(self._config_path, "r", encoding="utf-8", newline="") as f:
                    reader = csv.reader(f)
                    for row in reader:
                        if not row or len(row) < 2:
                            continue
                        k = row[0].strip()
                        v = row[1].strip()
                        if k.lower() == key_name.strip().lower():
                            rows.append([k, value])
                            found = True
                        else:
                            rows.append([k, v])

            if not found:
                rows.append([key_name, value])

            with open(self._config_path, "w", encoding="utf-8", newline="") as f:
                writer = csv.writer(f)
                writer.writerows(rows)
        except Exception as e:
            print("Error saving config:", e)

    def load_config(self):
        try:
            with open(self._config_path, 'r', encoding='utf-8', newline='') as file:
                reader = csv.reader(file)
                for row in reader:
                    if len(row) < 2:
                        continue
                    key = row[0].strip().lower()
                    val = row[1].strip()
                    if key == "name":
                        self.name.setText(val)
                    elif key == "vendor code":
                        self.dept.setText(val)
                    elif key == "part code":
                        self.company.setText(val)
                    elif key == "com port":
                        self._saved_com_port = val
        except Exception as e:
            print("Error reading config:", e)

    def load_counter(self):
        today = self._today_str()
        file_date = self._get_data_csv_date()

        if file_date != today:
            # File counter thuộc ngày cũ -> reset
            self.ok_count_value = self.ng_count_value = self.total_count_value = 0
            self.counter_date = today
            self.save_counter()   # cập nhật mtime sang hôm nay + lưu 0000
            return

        # Ngày trùng hôm nay -> đọc giá trị cũ nếu có
        if os.path.exists(self._counter_path):
            try:
                with open(self._counter_path, "r", newline='') as f:
                    reader = csv.reader(f)
                    for row in reader:
                        if row[0] == "OK":
                            self.ok_count_value = int(row[1])
                        elif row[0] == "NG":
                            self.ng_count_value = int(row[1])
                        elif row[0] == "Total":
                            self.total_count_value = int(row[1])
            except Exception:
                self.ok_count_value = self.ng_count_value = self.total_count_value = 0
        else:
            self.ok_count_value = self.ng_count_value = self.total_count_value = 0

        # Ghi nhận mốc ngày dùng để so sánh trong lúc chạy
        self.counter_date = today

    def save_counter(self):
        """
        Lưu giá trị counter vào file data.csv
        Format:
            OK,xxxx
            NG,xxxx
            Total,xxxx
        """
        try:
            with open(self._counter_path, "w", newline='') as f:
                writer = csv.writer(f)
                writer.writerow(["OK", f"{self.ok_count_value:04d}"])
                writer.writerow(["NG", f"{self.ng_count_value:04d}"])
                writer.writerow(["Total", f"{self.total_count_value:04d}"])
        except Exception as e:
            print("Error saving counter:", e)

    # ================== OTHER ==================
    def center_window(self):
        screen = QDesktopWidget().screenGeometry()
        x = (screen.width() - self.width()) // 2
        y = (screen.height() - self.height()) // 2
        self.move(x, y)

    def show_about_message(self):
        QMessageBox.information(self, "About", "Ver7.0\nFT Assy Charger Holder\nDec-17-2025")

    # def show_manual_message(self):
    #     QMessageBox.information(self, "Manual",
    #                             )

    def show_infor_message(self):
        QMessageBox.information(self, "Contact PIC", "songhung.tr\nVC/RD-Stick Team\nMobi: 03750311**")

    def on_combo_model_changed(self, text: str):
        """Hiển thị model đang chọn từ comboBox lên show_model."""
        if hasattr(self, "show_model") and self.show_model is not None:
            self.show_model.setText(text or "")

    def on_set_model_clicked(self):
        # giữ lại để tương thích (nếu UI cũ trỏ vào hàm này)
        self.handle_set_model_clicked()

    def handle_set_model_clicked(self):
        text = (self.show_model.text() if hasattr(self, "show_model") else "").strip()
        if not text:
            QMessageBox.warning(self, "Gửi model", "Giá trị model đang trống.")
            return
        if not self.serial_connection or not self.serial_connection.is_open:
            QMessageBox.critical(self, "Gửi model", "Chưa kết nối cổng COM.")
            self.set_model.setEnabled(False)
            return
        try:
            payload = f"MODEL={text}\n"
            self.serial_connection.write(payload.encode('utf-8'))
            self.serial_connection.flush()
            self.append_limited_log(f"[TX] {payload.strip()}")
            self.status.setText(f"Sent MODEL: {text}")
            self.status.setStyleSheet("color: green;")
        except Exception as e:
            self.status.setText(f"Lỗi gửi: {e}")
            self.status.setStyleSheet("color: red;")
            QMessageBox.critical(self, "Gửi model", f"Lỗi khi gửi: {e}")
        finally:
            # Khoá nút ngắn để tránh click liên tiếp
            self.set_model.setEnabled(False)
            QTimer.singleShot(250, lambda: self.set_model.setEnabled(True))


# ================== MAIN ==================
if __name__ == "__main__":
    app = QApplication(sys.argv)
    window = MyWindow()
    window.show()
    sys.exit(app.exec_())
