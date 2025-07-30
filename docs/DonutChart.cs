using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.ComponentModel.Design;   
using System.Drawing;
using System.Drawing.Design;         
using System.Linq;
using System.Windows.Forms;

namespace Donut
{

    [TypeConverter(typeof(ExpandableObjectConverter))]
    public class DonutSlice
    {
        public DonutSlice()
        {
            Label = "Yeni Dilim";
            Value = 10f;
            Color = Color.Gray;
        }

        [Category("Donut Slice"), Description("Bu dilimin gösterilecek metni (örneğin \"Kırmızı\").")]
        public string Label { get; set; }

        [Category("Donut Slice"), Description("Bu dilimin değeri. (Grafikte kaç dereceye karşılık geleceği hesaplanır.)")]
        public float Value { get; set; }

        [Category("Donut Slice"), Description("Bu dilimin rengi.")]
        public Color Color { get; set; }

        public override string ToString()
        {
            return $"{Label} ({Value:0})";
        }
    }

    public partial class DonutChart : UserControl
    {


        public DonutChart()
        {
            InitializeComponent();

            DoubleBuffered = true;          
            ResizeRedraw = true;            


            SetStyle(ControlStyles.Opaque, false);
            SetStyle(ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
        }

        private List<DonutSlice> _data = new List<DonutSlice>();

        [Category("Data"), Description("Grafikte gösterilecek dilimleri belirtir.")]
        [DesignerSerializationVisibility(DesignerSerializationVisibility.Content)]
        [Editor(typeof(CollectionEditor), typeof(UITypeEditor))]
        public List<DonutSlice> Data
        {
            get => _data;
            set
            {
                _data = value ?? new List<DonutSlice>();
                Invalidate();
            }
        }

        private float _donutThicknessRatio = 0.3f;
        [Category("Appearance"), Description("İç çemberin (donut deliğinin) kalınlığını dış yarıçapa oran olarak belirler (0..0.5).")]
        public float DonutThicknessRatio
        {
            get => _donutThicknessRatio;
            set
            {
                _donutThicknessRatio = Math.Max(0f, Math.Min(0.5f, value));
                Invalidate();
            }
        }

        private Control _overlayControl;
        [Category("Appearance"), Description(
            "Arka planı almak için kullanılacak kontrol. Eğer null bırakılırsa Parent kontrolü kullanılacaktır.")]
        public Control OverlayControl
        {
            get => _overlayControl;
            set
            {
                _overlayControl = value;
                Invalidate();
            }
        }

        protected override void OnPaintBackground(PaintEventArgs e)
        {
            // 1) Tasarım zamanı (DesignMode) kontrolü
            bool designMode = LicenseManager.UsageMode == LicenseUsageMode.Designtime
                           || (this.Site != null && this.Site.DesignMode);
            if (designMode)
            {
                // Designer’da normal arka planı çiz
                base.OnPaintBackground(e);
                return;
            }

            // 2) Sadece OverlayControl atandıysa ve BackColor transparent ise custom çizime izin ver
            if (OverlayControl == null || this.BackColor != Color.Transparent)
            {
                base.OnPaintBackground(e);
                return;
            }

            // 3) OverlayControl’ün görüntüsünü yakala ve kendi kontrol bölgen üzerinde çiz
            using (var bmp = new Bitmap(OverlayControl.Width, OverlayControl.Height))
            {
                OverlayControl.DrawToBitmap(bmp, new Rectangle(0, 0, bmp.Width, bmp.Height));

                // OverlayControl ile kendi konum farkını hesapla
                Point thisScreen = this.PointToScreen(Point.Empty);
                Point srcScreen = OverlayControl.PointToScreen(Point.Empty);
                int offsetX = thisScreen.X - srcScreen.X;
                int offsetY = thisScreen.Y - srcScreen.Y;

                // Çizim için kaynak ve hedef dikdörtgenleri
                Rectangle srcRect = new Rectangle(offsetX, offsetY, this.Width, this.Height);
                Rectangle destRect = new Rectangle(0, 0, this.Width, this.Height);

                e.Graphics.DrawImage(bmp, destRect, srcRect, GraphicsUnit.Pixel);
            }
        }


        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            DrawDonutChart(e.Graphics);
        }

        private void DrawDonutChart(Graphics g)
        {
            g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;

            if (_data == null || _data.Count == 0) return;
            float total = _data.Sum(d => d.Value);
            if (total <= 0) return;

            // --- Donut dış çemberi ---
            int w = ClientSize.Width;
            int h = ClientSize.Height;
            int margin = 20;
            int legendW = 140;
            int leftPad = margin;
            int topPad = margin;
            int rightPad = margin + legendW;
            int bottomPad = margin;
            int availW = w - leftPad - rightPad;
            int availH = h - topPad - bottomPad;
            int side = Math.Min(availW, availH);
            if (side <= 0) return;

            int x = leftPad;
            int y = topPad + (availH > availW ? (availH - side) / 2 : 0);
            var outerRect = new Rectangle(x, y, side, side);

            float angle = -90f;
            foreach (var slice in _data)
            {
                float sweep = slice.Value / total * 360f;
                using (var br = new SolidBrush(slice.Color))
                    g.FillPie(br, outerRect, angle, sweep);
                angle += sweep;
            }

            // --- İç delik (hole) ---
            float thickness = side * _donutThicknessRatio;
            var innerRect = new RectangleF(
                outerRect.X + thickness,
                outerRect.Y + thickness,
                outerRect.Width - 2 * thickness,
                outerRect.Height - 2 * thickness
            );

            // Panel veya OverlayControl'un BackColor'ı
            Color holeColor = (OverlayControl ?? this.Parent)?.BackColor ?? this.BackColor;
            using (var holeBrush = new SolidBrush(holeColor))
                g.FillEllipse(holeBrush, innerRect);

            // --- Yüzdeleri yaz ---
            angle = -90f;
            using (var fontP = new Font("Segoe UI", 9f, FontStyle.Bold))
            using (var sf = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center })
            {
                float rOut = side / 2f;
                float rIn = innerRect.Width / 2f;
                float rMid = (rOut + rIn) / 2f;
                var center = new PointF(outerRect.X + rOut, outerRect.Y + rOut);

                foreach (var slice in _data)
                {
                    float sweep = slice.Value / total * 360f;
                    float mid = angle + sweep / 2f;
                    float rad = mid * (float)Math.PI / 180f;
                    var pt = new PointF(
                        center.X + (float)(rMid * Math.Cos(rad)),
                        center.Y + (float)(rMid * Math.Sin(rad))
                    );

                    string txt = $"{slice.Value / total * 100:0}%";
                    using (var txtBrush = new SolidBrush(GetContrastingColor(slice.Color)))
                        g.DrawString(txt, fontP, txtBrush, pt, sf);

                    angle += sweep;
                }
            }

            // --- Legend ---
            float legendX = this.ClientSize.Width - 160;   // eğer sabit offset kullandıysan
            float legendY = outerRect.Y;
            using (var fontL = new Font("Segoe UI", 9f, FontStyle.Bold))
            {
                for (int i = 0; i < _data.Count; i++)
                {
                    var slice = _data[i];
                    float perc = slice.Value / total * 100f;
                    string text = $"{slice.Label}: {slice.Value:0} ({perc:0}%)";

                    var box = new RectangleF(legendX, legendY + i * 20, 10, 10);
                    using (var br = new SolidBrush(slice.Color))
                        g.FillRectangle(br, box);
                    g.DrawRectangle(Pens.Gray, Rectangle.Round(box));
                    g.DrawString(text, fontL, Brushes.Black, box.Right + 4, box.Y - 1);
                }
            }
        }




        private Color GetContrastingColor(Color bg)
        {
            double luminance = (0.299 * bg.R + 0.587 * bg.G + 0.114 * bg.B) / 255;
            return luminance > 0.5 ? Color.Black : Color.White;
        }
    }
}
