import os
import sys
import csv
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
from openpyxl import Workbook, load_workbook
import qrcode


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
        loadUi("gui2.ui", self)
        self.setWindowTitle("FT Assy Charger Base")

        icon_path = 'stick.png'
        icon_pixmap = QPixmap(icon_path).scaled(90, 100)
        self.setWindowIcon(QIcon(icon_pixmap))
        self.setFixedSize(1380, 670)
        self.center_window()

        # init Table
        self.data_table.setColumnCount(4)
        self.data_table.setHorizontalHeaderLabels(["No.", "Time", "ADC Value", "Result"])

        # Load config + counter
        self.load_config()
        self.load_counter()

        self.ok_count.setText(f"{self.ok_count_value:04d}")
        self.ng_count.setText(f"{self.ng_count_value:04d}")
        self.total_count.setText(f"{self.total_count_value:04d}")

        # Khởi tạo biến
        self.serial_connection = None
        self.last_state = "NONE"

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
        self.actionManual.triggered.connect(self.show_manual_message)
        self.actionVer.triggered.connect(self.show_about_message)
        self.actionInfor.triggered.connect(self.show_infor_message)

        # ports
        self.populate_com_ports()

        # init state
        self.reset_sensors()
        self.display.setPlainText("")
        self.status.setReadOnly(True)
        self.qr_print.setReadOnly(True)
        self.dept.setReadOnly(True)
        self.company.setReadOnly(True)

    # ================== TIME UPDATE ==================
    # def update_time(self):
    #     current_datetime = QDateTime.currentDateTime()
    #     self.label_time.setText(current_datetime.toString("HH:mm:ss"))
    #     self.label_date.setText(current_datetime.toString("dd-MM-yyyy"))

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

    # ================== COM PORT ==================
    def populate_com_ports(self):
        ports = serial.tools.list_ports.comports()
        self.comboBox_com_ports.clear()
        for port in ports:
            self.comboBox_com_ports.addItem(port.device)

    def connect_com(self):
        selected_port = self.comboBox_com_ports.currentText()
        if selected_port:
            if self.serial_connection and self.serial_connection.is_open:
                self.serial_connection.close()
                self.comboBox_com_ports.setEnabled(True)
                self.status.setText(f"Disconnected from {selected_port}")
                self.status.setStyleSheet("color: orange;")
            else:
                try:
                    self.serial_connection = serial.Serial(selected_port, 115200, timeout=1)
                    self.comboBox_com_ports.setEnabled(False)
                    self.status.setText(f"Connected to {selected_port} - 115200")
                    self.status.setStyleSheet("color: green;")
                except serial.SerialException as e:
                    self.status.setText(f"Failed to connect: {e}")
                    self.status.setStyleSheet("color: red;")

    def update_serial_port(self):
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
                self.serial_connection = serial.Serial(selected_port, 115200, timeout=1)
                self.comboBox_com_ports.setEnabled(False)
                self.status.setText(f"Reconnected to {selected_port}")
                self.status.setStyleSheet("color: green;")
            except serial.SerialException:
                self.comboBox_com_ports.setEnabled(True)
                self.status.setText("Failed to reconnect")
                self.status.setStyleSheet("color: red;")

    # ================== SERIAL READ ==================
    def read_from_com(self):
        if not self.serial_connection or not self.serial_connection.is_open:
            return
        try:
            if self.serial_connection.in_waiting > 0:
                line = self.serial_connection.readline().decode('utf-8', errors='ignore').strip()
                if line:
                    self.append_limited_log(line)
                    self.process_line(line)
        except serial.SerialException:
            self.status.setText("Lỗi cổng COM")
            self.status.setStyleSheet("color: red;")
            self.reconnect_com()

    # def process_line(self, line):
    def process_line(self, line):
        if line.startswith("Waiting"):
            self.value.setText("Wait")
            self.value.setStyleSheet(COLOR_WAIT)
            self.reset_sensors()

        elif line.startswith("OK"):
            self.value.setText("OK")
            self.value.setStyleSheet(COLOR_OK)
            self.reset_sensors()

            # ✅ Tăng biến OK
            self.ok_count_value += 1
            self.ok_count.setText(f"{self.ok_count_value:04d}")

            # ✅ Cập nhật total
            self.total_count_value = self.ok_count_value + self.ng_count_value
            self.total_count.setText(f"{self.total_count_value:04d}")

            if "data=" in line:
             adc_str = line.split("data=")[1]
             self.value_adc.setText(adc_str)

            # ✅ Tạo QR code khi OK
            self.make_qr_code1()

            # ✅ In QR code luôn (nếu không disable)
            self.print_qr_code()

            self.save_qlineedit_to_csv()
            self.save_counter()

        elif line.startswith("NG"):
            self.value.setText("NG")
            self.value.setStyleSheet(COLOR_NG)
            self.reset_sensors()

            # ✅ Tăng biến NG
            self.ng_count_value += 1
            self.ng_count.setText(f"{self.ng_count_value:04d}")

            # ✅ Cập nhật total
            self.total_count_value = self.ok_count_value + self.ng_count_value
            self.total_count.setText(f"{self.total_count_value:04d}")

            # ✅ Hiển thị ADC value
            if "data=" in line:
                adc_str = line.split("data=")[1]
                self.value_adc.setText(adc_str)

            self.save_qlineedit_to_csv()
            self.save_counter()

            # highlight sensor NG
            values = adc_str.split(",")
            for i, val in enumerate(values, start=1):
                label = getattr(self, f"sc_{i}")
                if val.strip() == "1":
                    label.setStyleSheet(COLOR_RED)


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
        # year = datetime.now().strftime("%Y")
        # month = datetime.now().strftime("%m")
        # day = datetime.now().strftime("%d")

        year = self.year_display
        month = self.month_display
        day = self.day_display 

        data3 = self.dept.text()
        data4 = self.company.text()
        additional_text = self.comboBox.currentText()
        counter_str = f"{self.ok_count_value:04d}"

        qr_data = "".join(["18", additional_text, data3, year[-1], month, day, counter_str])
        self.qr_print.setText(qr_data)
        qr = qrcode.QRCode(version=1, box_size=12, border=2)
        qr.add_data(qr_data)
        qr.make(fit=True)
        qr_pil_image = qr.make_image(fill_color="black", back_color="white")
        qr_pil_image = qr_pil_image.resize((236, 236))

        qr_qimage = QImage(qr_pil_image.size[0], qr_pil_image.size[1], QImage.Format_ARGB32)
        for x in range(qr_pil_image.size[0]):
            for y in range(qr_pil_image.size[1]):
                color = qr_pil_image.getpixel((x, y))
                qr_qimage.setPixelColor(x, y, QColor(color, color, color))

        combined_pixmap = QPixmap(256, 295)
        combined_pixmap.fill(Qt.white)
        painter = QPainter(combined_pixmap)
        painter.drawImage(10, 0, qr_qimage)
        painter.setFont(QFont("SamsungSharpSans-Bold", 18))
        painter.setPen(Qt.black)
        text_width = painter.fontMetrics().horizontalAdvance(additional_text)
        text_x = (236 - text_width) // 2 + 10
        painter.drawText(text_x, 255, additional_text)
        painter.end()

        combined_pixmap.save("qr_code.png")
        self.qr_image = combined_pixmap
        self.label_qr_code.setPixmap(combined_pixmap.scaled(self.label_qr_code.size(),
                                                            Qt.KeepAspectRatio,
                                                            Qt.SmoothTransformation))

    def print_qr_code(self):
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
    # def save_qlineedit_to_csv(self):
    def save_qlineedit_to_csv(self):
        read_adc = self.value_adc.text()  # Giá trị ADC
        status = self.value.text()        # Trạng thái OK/NG

        os.makedirs("data", exist_ok=True)
        save_path = os.path.join("data", f"adc_data_{datetime.now().strftime('%Y-%m-%d')}.csv")

        file_exists = os.path.exists(save_path)

        try:
            with open(save_path, mode='a', newline='', encoding='utf-8') as file:
                writer = csv.writer(file)

                # Nếu file mới tạo, ghi dòng tiêu đề
                if not file_exists:
                    writer.writerow(["No.", "Date", "Time", "ADC Value", "Status"])

                # Đếm số dòng hiện tại để xác định STT
                with open(save_path, mode='r', encoding='utf-8') as count_file:
                    row_count = sum(1 for row in count_file) - 1  # Trừ 1 vì có dòng tiêu đề

                current_date = datetime.now().strftime("%Y-%m-%d")
                current_time = datetime.now().strftime("%H:%M:%S")

                writer.writerow([row_count + 1, current_date, current_time, read_adc, status])

        except Exception as e:
            QMessageBox.critical(self, "Lỗi", f"Không thể lưu dữ liệu: {str(e)}")


    # ================== RESET ==================
    def reset_sensors(self):
        for i in range(1, 5):
            label = getattr(self, f"sc_{i}")
            label.setStyleSheet(COLOR_BLUE)

    # ================== COUNTER + CONFIG ==================
    def load_config(self):
        try:
            with open('config.csv', 'r') as file:
                reader = csv.reader(file)
                for row in reader:
                    if row[0] == "name":
                        self.name.setText(row[1])
                    elif row[0] == "vendor code":
                        self.dept.setText(row[1])
                    elif row[0] == "part code":
                        self.company.setText(row[1])
        except Exception as e:
            print("Error reading config:", e)

    def load_counter(self):
        if os.path.exists("data.csv"):
            try:
                with open("data.csv", "r", newline='') as f:
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
    def save_counter(self):
        """
        Lưu giá trị counter vào file data.csv
        Format:
            OK,xxxx
            NG,xxxx
            Total,xxxx
        """
        try:
            with open("data.csv", "w", newline='') as f:
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
        QMessageBox.information(self, "About", "Ver2.0\nFT Assy Charger Base\nOct-25")

    def show_manual_message(self):
        QMessageBox.information(self, "Manual",
                                "1111: OK\n0100: NG_Trên_Phải\n0111: NG_Trên_Trái\n"
                                "0010: NG_Dưới_Phải\n1110: NG_Dưới_Phải\n"
                                "1010: NG_Ngược, Out 2Pin Dưới\n0101: NG_Ngược, Out 2Pin Trên")

    def show_infor_message(self):
        QMessageBox.information(self, "Contact PIC", "songhung.tr\nVC/RD-Stick Team\nMobi: 03750311**")


# ================== MAIN ==================
if __name__ == "__main__":
    app = QApplication(sys.argv)
    window = MyWindow()
    window.show()
    sys.exit(app.exec_())
