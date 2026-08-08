/*
 * compat.h - Linux / OpenBSD の差分吸収（SPEC §1.0, §2.2.1, Phase 5）
 *
 * 方針: 条件コンパイルはこのヘッダに閉じ込め、他のソースには #ifdef を書かない。
 */
#ifndef COMPAT_H
#define COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ------------------------------------------------------------------ *
 * strlcpy / strlcat
 *   OpenBSD には標準である。glibc は 2.38 以降で提供する。
 *   無い環境向けに util.c が実装を持つ。
 * ------------------------------------------------------------------ */
#if defined(__OpenBSD__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__APPLE__)
#  include <string.h>
#  define WM_HAVE_STRLCPY 1
#elif defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#  if __GLIBC_PREREQ(2, 38)
#    include <string.h>
#    define WM_HAVE_STRLCPY 1
#  endif
#endif

#ifndef WM_HAVE_STRLCPY
size_t wm_strlcpy(char *dst, const char *src, size_t dsize);
size_t wm_strlcat(char *dst, const char *src, size_t dsize);
#  define strlcpy wm_strlcpy
#  define strlcat wm_strlcat
#endif

/* ------------------------------------------------------------------ *
 * pledge / unveil (OpenBSD)
 *   他 OS では何もしない。呼び出し側は常に呼んでよい。
 * ------------------------------------------------------------------ */
#ifdef __OpenBSD__
#  include <unistd.h>
#  define wm_pledge(promises, execpromises) pledge((promises), (execpromises))
#  define wm_unveil(path, permissions)      unveil((path), (permissions))
#else
#  define wm_pledge(promises, execpromises) (0)
#  define wm_unveil(path, permissions)      (0)
#endif

/* ------------------------------------------------------------------ *
 * 単調時刻（ミリ秒）
 *   ドラッグのレート制御（SPEC §3.4.1）と sync タイムアウト（§7.3）に使う。
 *   実装は util.c。CLOCK_MONOTONIC を使い、システム時刻の変更に影響されない。
 * ------------------------------------------------------------------ */
uint64_t wm_now_ms(void);

/* ------------------------------------------------------------------ *
 * OS 名（ログ用）
 * ------------------------------------------------------------------ */
#if defined(__linux__)
#  define WM_OS_NAME "Linux"
#elif defined(__OpenBSD__)
#  define WM_OS_NAME "OpenBSD"
#else
#  define WM_OS_NAME "unknown"
#endif

#endif /* COMPAT_H */
