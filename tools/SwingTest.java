/*
 * SwingTest.java - ICCCM の Globally Active 入力モデルを踏むための検証用アプリ
 *
 *   java tools/SwingTest.java        (JDK 11 以降。javac 不要)
 *
 * なぜ Swing なのか
 * ----------------
 * ICCCM §4.1.7 は入力モデルを 4 つ定めている:
 *
 *   input  take_focus  モデル              WM がすること
 *   ---------------------------------------------------------------
 *   True   False       Passive             SetInputFocus を呼ぶ
 *   True   True        Locally Active      SetInputFocus + WM_TAKE_FOCUS
 *   False  True        Globally Active     **SetInputFocus を呼んではならない**
 *                                          WM_TAKE_FOCUS を送るだけ
 *   False  False       No Input            何もしない
 *
 * このうち Globally Active を実際に使う実装は少なく、Java/AWT がその代表。
 * GTK も Qt も Passive なので、他のツールキットでいくらテストしても
 * この経路には入らない。取り違えると「クリックしてもキー入力が入らない
 * ウィンドウ」ができるが、Swing アプリを開くまで誰も気づかない。
 *
 * 確認すること
 * ------------
 *   1. 下のテキスト欄をクリックして文字が打てる
 *   2. 打てたら、他のウィンドウをクリックしてから戻ってきて、また打てる
 *   3. ダイアログを開いて閉じたあと、親のテキスト欄にまた打てる
 *   4. 画面上部のラベルが "フォーカス: あり" に変わる
 *
 * 4 が「あり」なのに 1 で文字が打てない場合、WM が SetInputFocus を
 * 呼んでしまっている（= Globally Active を Passive として扱っている）。
 */
import java.awt.BorderLayout;
import java.awt.Dimension;
import java.awt.event.WindowAdapter;
import java.awt.event.WindowEvent;
import javax.swing.BorderFactory;
import javax.swing.Box;
import javax.swing.BoxLayout;
import javax.swing.JButton;
import javax.swing.JDialog;
import javax.swing.JFrame;
import javax.swing.JLabel;
import javax.swing.JPanel;
import javax.swing.JScrollPane;
import javax.swing.JTextArea;
import javax.swing.JTextField;
import javax.swing.SwingUtilities;

public class SwingTest {

    private static JLabel focusLabel;

    private static void showModalDialog(JFrame parent) {
        JDialog d = new JDialog(parent, "モーダルダイアログ", true);
        JPanel p = new JPanel();
        p.setLayout(new BoxLayout(p, BoxLayout.Y_AXIS));
        p.setBorder(BorderFactory.createEmptyBorder(12, 12, 12, 12));
        p.add(new JLabel("これはモーダルです。親は操作できないはず。"));
        p.add(Box.createVerticalStrut(8));
        p.add(new JTextField("ここにも入力できること", 24));
        p.add(Box.createVerticalStrut(8));
        JButton close = new JButton("閉じる");
        close.addActionListener(e -> d.dispose());
        p.add(close);
        d.getContentPane().add(p);
        d.pack();
        d.setLocationRelativeTo(parent);
        d.setVisible(true);
    }

    private static void showModelessDialog(JFrame parent) {
        JDialog d = new JDialog(parent, "非モーダルダイアログ", false);
        JPanel p = new JPanel();
        p.setBorder(BorderFactory.createEmptyBorder(12, 12, 12, 12));
        p.add(new JLabel("親も同時に操作できるはず。"));
        d.getContentPane().add(p);
        d.pack();
        d.setLocation(parent.getX() + 60, parent.getY() + 60);
        d.setVisible(true);
    }

    private static void showFixedSizeWindow(JFrame parent) {
        JDialog d = new JDialog(parent, "リサイズ不可", false);
        d.setResizable(false);      /* min==max のヒントになる */
        JPanel p = new JPanel();
        p.setBorder(BorderFactory.createEmptyBorder(12, 12, 12, 12));
        p.add(new JLabel("枠を掴んでもサイズが変わらないこと。枠は 3px の細枠。"));
        d.getContentPane().add(p);
        d.pack();
        d.setLocation(parent.getX() + 120, parent.getY() + 120);
        d.setVisible(true);
    }

    public static void main(String[] args) {
        SwingUtilities.invokeLater(() -> {
            JFrame f = new JFrame("Swing 入力モデルの検証 — 日本語タイトル");
            f.setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);

            JPanel root = new JPanel(new BorderLayout(8, 8));
            root.setBorder(BorderFactory.createEmptyBorder(10, 10, 10, 10));

            focusLabel = new JLabel("フォーカス: なし");
            root.add(focusLabel, BorderLayout.NORTH);

            JTextArea area = new JTextArea(
                "ここをクリックして文字を打ってください。\n"
                + "打てなければ、WM が ICCCM の Globally Active を\n"
                + "誤って Passive として扱っています。\n\n");
            area.setLineWrap(true);
            JScrollPane sp = new JScrollPane(area);
            sp.setPreferredSize(new Dimension(460, 220));
            root.add(sp, BorderLayout.CENTER);

            JPanel buttons = new JPanel();
            JButton b1 = new JButton("モーダル");
            b1.addActionListener(e -> showModalDialog(f));
            JButton b2 = new JButton("非モーダル");
            b2.addActionListener(e -> showModelessDialog(f));
            JButton b3 = new JButton("リサイズ不可の窓");
            b3.addActionListener(e -> showFixedSizeWindow(f));
            JButton b4 = new JButton("全画面");
            b4.addActionListener(e -> {
                f.dispose();
                f.setUndecorated(!f.isUndecorated());
                f.setExtendedState(f.getExtendedState() == JFrame.MAXIMIZED_BOTH
                                   ? JFrame.NORMAL : JFrame.MAXIMIZED_BOTH);
                f.setVisible(true);
            });
            buttons.add(b1);
            buttons.add(b2);
            buttons.add(b3);
            buttons.add(b4);
            root.add(buttons, BorderLayout.SOUTH);

            f.getContentPane().add(root);
            f.pack();
            f.setLocationByPlatform(true);

            /*
             * 打鍵が実際にアプリまで届いたことを標準出力に出す。
             * 結合テスト (130-swing-globally-active.sh) がこれを見て
             * 「フォーカスは渡ったが入力が入らない」状態を検出する。
             * 目視で使うときも邪魔にはならない。
             */
            area.addKeyListener(new java.awt.event.KeyAdapter() {
                public void keyTyped(java.awt.event.KeyEvent e) {
                    System.out.println("KEY:" + e.getKeyChar());
                    System.out.flush();
                }
            });

            f.addWindowFocusListener(new java.awt.event.WindowFocusListener() {
                public void windowGainedFocus(WindowEvent e) {
                    focusLabel.setText("フォーカス: あり  "
                        + "(ここが「あり」なのに打てなければ WM 側の問題)");
                    System.out.println("FOCUS:gained");
                    System.out.flush();
                }
                public void windowLostFocus(WindowEvent e) {
                    focusLabel.setText("フォーカス: なし");
                    System.out.println("FOCUS:lost");
                    System.out.flush();
                }
            });
            f.addWindowListener(new WindowAdapter() {
                public void windowClosing(WindowEvent e) {
                    System.out.println("WM_DELETE_WINDOW を受け取りました");
                }
            });

            f.setVisible(true);
            area.requestFocusInWindow();
        });
    }
}
