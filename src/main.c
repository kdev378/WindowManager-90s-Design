/*
 * main.c - 起動・シグナル・イベントループ
 *
 * 対応: SPEC §2.2.1(シグナル), §3.4.1(ペーシング), §7.3(タイムアウト), §9.2
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "w98wm.h"

/* シグナルハンドラから触れるのはこの 2 つだけ */
static volatile sig_atomic_t got_term;
static volatile sig_atomic_t got_hup;

/* ------------------------------------------------------------------ *
 * シグナル (SPEC §2.2.1)
 *
 * SIGCHLD は SIG_IGN。POSIX により子はゾンビにならないので waitpid も
 * SIGCHLD 用の self-pipe も不要。SIGPIPE も SIG_IGN。
 * どちらも exec を跨いで継承されるため、子プロセス側では
 * posix_spawn の POSIX_SPAWN_SETSIGDEF で既定に戻す (input.c)。
 *
 * self-pipe で受けるのは SIGTERM / SIGINT / SIGHUP のみ。
 * ハンドラは write(2) 1 バイトだけ。パイプ満杯 (EAGAIN) は無視してよい
 * ——この通知は「何か来た」というエッジの合体で足りるため。
 * ------------------------------------------------------------------ */
static void sig_handler(int sig)
{
	unsigned char b = 1;
	ssize_t n;

	if (sig == SIGHUP)
		got_hup = 1;
	else
		got_term = 1;

	n = write(wm.sig_pipe[1], &b, 1);
	(void)n;   /* EAGAIN は無視。意図的 */
}

static bool setup_signals(void)
{
	struct sigaction sa;
	int i;

	if (pipe(wm.sig_pipe) < 0)
		return false;
	for (i = 0; i < 2; i++) {
		int fl = fcntl(wm.sig_pipe[i], F_GETFL, 0);
		fcntl(wm.sig_pipe[i], F_SETFL, fl | O_NONBLOCK);
		fl = fcntl(wm.sig_pipe[i], F_GETFD, 0);
		fcntl(wm.sig_pipe[i], F_SETFD, fl | FD_CLOEXEC);
	}

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = sig_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT,  &sa, NULL);
	sigaction(SIGHUP,  &sa, NULL);

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = SIG_IGN;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGCHLD, &sa, NULL);   /* POSIX: 子はゾンビにならない */
	sigaction(SIGPIPE, &sa, NULL);
	return true;
}

static void drain_sig_pipe(void)
{
	unsigned char buf[64];
	while (read(wm.sig_pipe[0], buf, sizeof buf) > 0)
		;
}

/* ------------------------------------------------------------------ *
 * イベントループ
 *
 * fd は X 接続と self-pipe の 2 本だけ。タイマスレッドもタイマ fd も
 * 使わず、poll のタイムアウトで §3.4.1 のレート制御と §7.3 の
 * 250ms タイムアウトを賄う。
 * ------------------------------------------------------------------ */
static void event_loop(void)
{
	struct pollfd pfd[2];
	int xfd = xcb_get_file_descriptor(wm.conn);

	pfd[0].fd = xfd;
	pfd[0].events = POLLIN;
	pfd[1].fd = wm.sig_pipe[0];
	pfd[1].events = POLLIN;

	/*
	 * アイドル時の malloc_trim (SPEC §9.2)
	 *
	 * クライアントを管理に取り込むとき、プロパティ読み取りと xcb の
	 * リプライで一時領域を大量に確保しては解放する。解放しても glibc の
	 * アリーナは縮まないので、**使っていない**のに Private_Dirty に
	 * 残り続ける。実測では 20 窓を一気に開くとヒープが 40→76 KB に育ち、
	 * その差 36 KB がまるごとこれだった（窓あたり 1.8 KB 相当で、
	 * §9.1 の 2 KB/窓ゲートの大半を占めていた）。
	 *
	 * 何もイベントが来ない状態が続いたら 1 回だけ返す。ドラッグ中や
	 * 連続してウィンドウが開いている最中には走らないので、
	 * 「返した直後にまた確保する」振動は起きない。
	 * glibc 以外では no-op。
	 */
	bool idle_trimmed = false;

	while (wm.running) {
		xcb_generic_event_t *ev;
		uint64_t now;
		int timeout, ret;
		bool saw_event = false;

		/* 送信待ちを掃き出してからブロックする（忘れると固まる） */
		xcb_flush(wm.conn);

		now = wm_now_ms();
		timeout = move_next_timeout_ms(now);
		/*
		 * ping と起動通知のタイムアウトは秒単位なので、move の
		 * ミリ秒精度の要求より粗い。待ちなし(-1)の時だけ 1 秒で起こす。
		 */
		if (timeout < 0)
			timeout = 1000;   /* 時計 (§4.8) と ping/起動通知の粗い刻み */

		ret = poll(pfd, 2, timeout);
		if (ret < 0 && errno != EINTR) {
			ERR("poll: %s", strerror(errno));
			break;
		}

		if (pfd[1].revents & POLLIN) {
			drain_sig_pipe();
			if (got_term) {
				LOG("終了シグナルを受信");
				wm.running = false;
				break;
			}
			if (got_hup) {
				got_hup = 0;
				LOG("設定を再読み込み");
				config_free(&wm.cfg);
				config_load(&wm.cfg);
				theme_init();          /* 配色とメトリクスを取り直す */
				input_regrab_keys();
				/*
				 * 配色の変更はここで全面に効く。ただしタスクバーの
				 * 高さ (scale 由来) だけは作り直しが要るため反映しない
				 * ——トレイに埋め込み済みのアイコンを巻き込むため。
				 * scale の変更は再起動が必要 (README に記載)。
				 */
				taskbar_update_strut();
				taskbar_update();
				{
					struct client *rc;
					for (rc = wm.stack_bottom; rc != NULL; rc = rc->next)
						if (rc->flags & CF_DECORATED)
							deco_draw(rc, NULL);
				}
			}
		}

		while ((ev = xcb_poll_for_event(wm.conn)) != NULL) {
			saw_event = true;
			event_dispatch(ev);
			free(ev);
			if (!wm.running)
				break;
		}

		if (saw_event) {
			idle_trimmed = false;
		} else if (!idle_trimmed && !move_active()) {
			idle_trimmed = true;
#ifdef __GLIBC__
			malloc_trim(0);
#endif
		}

		if (xcb_connection_has_error(wm.conn)) {
			ERR("X 接続が切れた");
			break;
		}

		{
			uint64_t t = wm_now_ms();
			move_tick(t);
			taskbar_tick(t);           /* §4.8 時計と点滅 */
			sync_check_timeout(t);     /* §7.3 250ms */
			ping_check_timeout(t);     /* §7.4 5s */
			startup_check_timeout(t);  /* §7.4 15s */
		}
	}
}

int main(int argc, char **argv)
{
	int i, verbose = 0, print_font_only = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0) {
			verbose = 1;
		} else if (strcmp(argv[i], "--version") == 0) {
			printf("%s %s\n", WM_DISPLAY_NAME, WM_VERSION);
			return 0;
		} else if (strcmp(argv[i], "--print-font") == 0) {
			/* §4.5.1: どの候補が採用されたかを確認する手段 */
			print_font_only = 1;
		} else if (strcmp(argv[i], "--help") == 0) {
			printf("usage: %s [-v] [--replace] [--version] [--print-font]\n",
			       argv[0]);
			return 0;
		}
	}

	log_init(verbose);
	config_defaults(&wm.cfg);
	config_load(&wm.cfg);
	theme_init();          /* メトリクスと配色。設定のみに依存する */

	if (!setup_signals()) {
		ERR("シグナルの初期化に失敗");
		return 1;
	}
	if (!wm_init(argc, argv))
		return 1;

	if (print_font_only) {
		printf("%s\n", font_describe());
		wm_shutdown();
		return 0;
	}

	/* タスクバーとトレイ (§4.8)。既存クライアントを採用し終えた後に立てる */
	taskbar_init();
	tray_init();
	taskbar_update();

	/*
	 * OpenBSD の権限縮小 (SPEC Phase 5)。X 接続確立後に絞る。
	 * proc/exec はキーバインドからのアプリ起動に必要。
	 * 他 OS では no-op (compat.h)。
	 */
	if (wm_pledge("stdio rpath unix proc exec", NULL) < 0)
		ERR("pledge に失敗した");

	/* 起動時の一時領域を返す (SPEC §9.2)。glibc のみ */
#ifdef __GLIBC__
	malloc_trim(0);
#endif

	event_loop();

	menu_close();
	switcher_end(true);
	tray_fini();
	taskbar_fini();
	cursor_fini();
	font_fini();
	draw_fini();
	wm_shutdown();
	return 0;
}
