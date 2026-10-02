from pathlib import Path
import fitz
from reportlab.pdfgen import canvas
from reportlab.lib.colors import HexColor
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfbase.pdfmetrics import stringWidth

W,H = 419.528,595.276
BASE = Path('/tmp/MAYAP_Huong_dan_van_hanh_A5_v1.2_E502.pdf')
APP = Path('/tmp/MAYAP_append_v1.3.pdf')
OUT = Path('docs/MAYAP_Huong_dan_van_hanh_A5_v1.3_E503.pdf')
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
BOLD = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
pdfmetrics.registerFont(TTFont('DV', FONT))
pdfmetrics.registerFont(TTFont('DVB', BOLD))
ink=HexColor('#22303A'); muted=HexColor('#758690'); teal=HexColor('#0E958E'); line=HexColor('#D8E2E3'); fill=HexColor('#F7F9F9'); amber=HexColor('#D58D00')

cards = [
('E104','DỪNG AN TOÀN','Cảm biến nhiệt bị đứng',
 'Giá trị nhiệt độ (PV) đứng bất thường trong khi hệ thống đã gia nhiệt một khoảng thời gian đáng kể. Máy coi dữ liệu cảm biến không còn đáng tin cậy và ngắt gia nhiệt để bảo vệ.',
 ['Kiểm tra cảm biến nhiệt, nguồn cấp và dây/bus giao tiếp.','Đối chiếu nhiệt độ bằng nhiệt kế độc lập.','Chỉ cho máy chạy lại khi giá trị PV thay đổi bình thường và ổn định.'],'stop'),
('E138','DỪNG AN TOÀN','Mẻ quá hạn đang chờ xác nhận',
 'Mẻ đã vượt số ngày ấp dự kiến và đang chờ người vận hành xác nhận tiếp tục hay kết thúc. Hệ thống giữ trạng thái an toàn trong thời gian chờ quyết định.',
 ['Kiểm tra tuổi mẻ và tình trạng trứng thực tế.','Trên HMI xác nhận tiếp tục ủ ấm hoặc kết thúc mẻ.','Không bỏ qua cảnh báo nếu số ngày ấp không còn đúng với mẻ thực tế.'],'stop'),
('E501','CẢNH BÁO','Mất kết nối bộ cảnh báo mất điện',
 'ESP32 không nhận được ACK/trạng thái từ ATtiny13A của bộ còi mất điện. Lỗi này không tác động trực tiếp tới điều khiển nhiệt hoặc đảo trứng.',
 ['Kiểm tra nguồn ATtiny và nguồn/pin dự phòng của còi.','Kiểm tra dây BUS 1-wire, đầu nối và mức tín hiệu 3.3 V.','Sau khi sửa, thử lại giao tiếp và còi cảnh báo mất điện.'],'warn'),
('E503','CẢNH BÁO','Trạng thái ESP32 - ATtiny chưa đồng bộ',
 'Đường giao tiếp ATtiny vẫn hoạt động nhưng trạng thái mẻ giữa ESP32 và ATtiny không khớp nhau. Cần đồng bộ lại để hệ cảnh báo mất điện biết đúng trạng thái mẻ.',
 ['Kiểm tra BUS và trạng thái mẻ hiện tại trên HMI.','Thực hiện lại thao tác bắt đầu/dừng mẻ để đồng bộ trạng thái nếu phù hợp.','Xác nhận ATtiny nhận đúng trạng thái trước khi bàn giao máy.'],'warn')]

def wrap(text,fontname,fontsize,maxw):
    words=text.split(); lines=[]; cur=''
    for w in words:
        test=(cur+' '+w).strip()
        if stringWidth(test,fontname,fontsize)<=maxw: cur=test
        else:
            if cur: lines.append(cur)
            cur=w
    if cur: lines.append(cur)
    return lines

def draw_card(c,y_top,code,level,title,meaning,bullets,kind):
    x=30; w=W-60; h=112; y=y_top-h
    c.setFillColor(HexColor('#FFFFFF')); c.setStrokeColor(line); c.setLineWidth(0.8)
    c.roundRect(x,y,w,h,14,fill=1,stroke=1)
    accent=teal if kind=='warn' else amber
    c.setFillColor(accent); c.roundRect(x+1.2,y+11,3.4,h-22,2,fill=1,stroke=0)
    badge=f'{code} · {level}'; bw=stringWidth(badge,'DVB',6.6)+18
    c.roundRect(x+14,y+h-25,bw,15,7.5,fill=1,stroke=0)
    c.setFillColor(HexColor('#FFFFFF')); c.setFont('DVB',6.6); c.drawString(x+23,y+h-20.4,badge)
    tx=x+14+bw+9; fs=9.0; max_title=W-30-tx
    while stringWidth(title,'DVB',fs)>max_title and fs>7.3: fs-=0.2
    c.setFillColor(ink); c.setFont('DVB',fs); c.drawString(tx,y+h-20.5,title)
    bx1=x+14; by=y+12; bh=68; bw1=115; gap=6; bx2=bx1+bw1+gap; bw2=w-28-bw1-gap
    c.setFillColor(fill); c.setStrokeColor(line); c.setLineWidth(0.7)
    c.roundRect(bx1,by,bw1,bh,11,fill=1,stroke=1); c.roundRect(bx2,by,bw2,bh,11,fill=1,stroke=1)
    c.setFillColor(muted); c.setFont('DVB',5.8); c.drawString(bx1+8,by+bh-12,'Ý NGHĨA'); c.drawString(bx2+8,by+bh-12,'XỬ LÝ')
    c.setFillColor(HexColor('#46555D')); c.setFont('DV',6.2); yy=by+bh-24
    for ln in wrap(meaning,'DV',6.2,bw1-16): c.drawString(bx1+8,yy,ln); yy-=8.0
    yy=by+bh-24
    for b in bullets:
        for ln in wrap('• '+b,'DV',6.0,bw2-16): c.drawString(bx2+8,yy,ln); yy-=7.4
        yy-=1.4

def main():
    c=canvas.Canvas(str(APP),pagesize=(W,H))
    c.setFont('DVB',7.4); c.setFillColor(muted); c.drawString(30,H-25,'MÁY ẤP TRỨNG CÔNG NGHIỆP')
    c.setFont('DV',7.0); c.drawRightString(W-30,H-25,'TECHNICAL GUIDE / A5')
    c.setStrokeColor(line); c.setLineWidth(0.5); c.line(30,H-35,W-30,H-35)
    for i,card in enumerate(cards): draw_card(c,H-46-i*124,*card)
    c.setStrokeColor(line); c.setLineWidth(0.45); c.line(30,22,148,22); c.line(W-37,22,W-30,22)
    c.setFillColor(muted); c.setFont('DV',6.2); c.drawString(30,8,'Bảng mã lỗi & hướng dẫn xử lý · Rev. 1.3'); c.drawRightString(W-30,8,'14')
    c.showPage(); c.save()
    doc=fitz.open(str(BASE)); add=fitz.open(str(APP)); doc.insert_pdf(add); OUT.parent.mkdir(parents=True,exist_ok=True); doc.save(str(OUT),garbage=4,deflate=True)

if __name__ == '__main__': main()
