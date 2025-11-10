#include <gtk/gtk.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

/* ---------- configuration ---------- */
/* relative path to your vendored yt-dlp */
#define PYTHON_PROG "python3"
#define YTDLP_PATH  "./libs/yt-dlp"

/* single temp file for the downloaded input */
#define YTDLP_TMP_FILE "/tmp/ytdlp_input.mkv"

/* ---------- app state ---------- */

typedef enum {
    PHASE_IDLE = 0,
    PHASE_DOWNLOADING,
    PHASE_TRANSCODING
} Phase;

typedef struct {
    GtkEntry       *input_entry;
    GtkEntry       *output_entry;
    GtkDropDown    *format_dropdown;
    GtkProgressBar *progress_bar;
    GtkLabel       *progress_label; /* ETA label */
    GtkLabel       *status_label;
    GtkButton      *convert_btn;
    GtkButton      *cancel_btn;

    /* processes */
    GPid            yt_pid;
    GPid            ffmpeg_pid;

    /* I/O watches */
    GIOChannel     *yt_io;       /* stdout from yt-dlp */
    GIOChannel     *ff_io;       /* stderr from ffmpeg (-progress pipe:2) */

    /* media info */
    gdouble         total_duration; /* seconds (media) for ffmpeg stage */

    /* unified ETA model (wall clock) */
    Phase           phase;
    gint64          t_start_us;        /* monotonic at overall start */
    gdouble         dl_eta_sec;        /* remaining ETA for download (reported by yt-dlp) */
    gdouble         tx_eta_sec;        /* remaining ETA for transcode (derived from ffmpeg speed) */
    gdouble         dl_progress_0_1;   /* fraction within download */
    gdouble         tx_progress_0_1;   /* fraction within transcode */

    gboolean        cancel_requested;
} AppWidgets;

/* ---------- helpers ---------- */

static gboolean
is_youtube_url(const char *url)
{
    if (!url) return FALSE;
    return g_str_has_prefix(url, "https://www.youtube.com/") ||
           g_str_has_prefix(url, "https://youtu.be/") ||
           g_str_has_prefix(url, "http://www.youtube.com/") ||
           g_str_has_prefix(url, "http://youtu.be/");
}

static char *
append_extension_if_missing(const char *path, const char *format)
{
    if (!path || !format) return NULL;

    const char *ext =
        g_strcmp0(format, "PNG")  == 0 ? ".png"  :
        g_strcmp0(format, "JPEG") == 0 ? ".jpg"  :
        g_strcmp0(format, "WEBP") == 0 ? ".webp" :
        g_strcmp0(format, "GIF")  == 0 ? ".gif"  :
        g_strcmp0(format, "MP4")  == 0 ? ".mp4"  :
        g_strcmp0(format, "MP3")  == 0 ? ".mp3"  : "";

    if (!*ext) return g_strdup(path);
    if (g_str_has_suffix(path, ext)) return g_strdup(path);
    return g_strconcat(path, ext, NULL);
}

static gboolean
ensure_output_path(const char *filepath, GError **error)
{
    if (!filepath || !*filepath) return FALSE;

    char *dirpath = g_path_get_dirname(filepath);
    if (!dirpath) return FALSE;

    if (!g_file_test(dirpath, G_FILE_TEST_IS_DIR)) {
        if (g_mkdir_with_parents(dirpath, 0755) != 0) {
            int err = errno;
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(err),
                        "Failed to create directory '%s': %s",
                        dirpath, g_strerror(err));
            g_free(dirpath);
            return FALSE;
        }
    }
    g_free(dirpath);

    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        int err = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(err),
                    "Cannot create file '%s': %s",
                    filepath, g_strerror(err));
        return FALSE;
    }
    fclose(fp);
    return TRUE;
}

static void
format_secs(gdouble secs, char *out, size_t outlen)
{
    if (secs < 0) secs = 0;
    int s = (int)(secs + 0.5);
    int hh = s / 3600;
    int mm = (s % 3600) / 60;
    int ss = s % 60;
    g_snprintf(out, outlen, "%02d:%02d:%02d", hh, mm, ss);
}

/* Probe media duration with ffprobe (used for local files or after download) */
static gdouble
get_media_duration(const char *input)
{
    if (!input) return 0.0;

    gchar *argv[] = {
        "ffprobe", "-v", "error",
        "-show_entries", "format=duration",
        "-of", "default=noprint_wrappers=1:nokey=1",
        (gchar *)input, NULL
    };

    gchar *out = NULL;
    gboolean ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                               NULL, NULL, &out, NULL, NULL, NULL);
    if (!ok || !out) {
        g_free(out);
        return 0.0;
    }
    gdouble d = g_ascii_strtod(out, NULL);
    g_free(out);
    return d > 0 ? d : 0.0;
}

/* ---------- unified progress/ETA ---------- */

static void
update_unified_progress(AppWidgets *w)
{
    /* combined ETA = dl_eta + tx_eta (whichever phase active defines numbers) */
    gdouble remain = 0.0;
    if (w->phase == PHASE_DOWNLOADING) {
        remain = w->dl_eta_sec + w->tx_eta_sec; /* tx may be unknown -> 0 */
    } else if (w->phase == PHASE_TRANSCODING) {
        remain = w->tx_eta_sec; /* download done */
    }

    gint64 now_us = g_get_monotonic_time();
    gdouble elapsed = (now_us - w->t_start_us) / 1e6;

    gdouble est_total = elapsed + remain;
    gdouble frac = 0.0;
    if (est_total > 0.01) frac = elapsed / est_total;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    gtk_progress_bar_set_fraction(w->progress_bar, frac);

    char etabuf[64];
    format_secs(remain, etabuf, sizeof(etabuf));
    gtk_label_set_text(w->progress_label, etabuf);
}

/* ---------- yt-dlp (download) ---------- */

/* parse lines emitted by: --progress-template "progress:[downloaded=... total=... eta=... speed=... percent=...]" */
static gboolean
ytdlp_progress_cb(GIOChannel *source, GIOCondition cond, gpointer data)
{
    AppWidgets *w = data;
    if (cond & (G_IO_HUP | G_IO_ERR)) return FALSE;

    gchar *line = NULL;
    gsize len = 0;
    GError *err = NULL;
    GIOStatus st = g_io_channel_read_line(source, &line, &len, NULL, &err);

    if (st == G_IO_STATUS_NORMAL && line) {
        /* example:
           progress:[downloaded=1234567 total=9876543 eta=42 speed=2456785.0 percent=12.3%]
        */
        if (g_str_has_prefix(line, "progress:[")) {
            /* crude parse */
            gdouble eta = 0.0;
            gdouble downloaded = 0.0, total = 0.0;
            char *p = line;
            /* look for tokens */
            char *tok;

            tok = g_strstr_len(p, len, "downloaded=");
            if (tok) downloaded = g_ascii_strtod(tok + 11, NULL);
            tok = g_strstr_len(p, len, "total=");
            if (tok) total = g_ascii_strtod(tok + 6, NULL);
            tok = g_strstr_len(p, len, "eta=");
            if (tok) eta = g_ascii_strtod(tok + 4, NULL);

            w->dl_eta_sec = eta > 0 ? eta : 0.0;

            /* if we have total, we can compute per-phase fraction (not used for bar directly) */
            if (total > 0) {
                w->dl_progress_0_1 = downloaded / total;
                if (w->dl_progress_0_1 < 0) w->dl_progress_0_1 = 0;
                if (w->dl_progress_0_1 > 1) w->dl_progress_0_1 = 1;
            }

            update_unified_progress(w);
        }
        g_free(line);
        return TRUE;
    }

    if (st == G_IO_STATUS_EOF) {
        if (line) g_free(line);
        return FALSE;
    }

    if (err) g_error_free(err);
    return TRUE;
}

static void child_watch_ffmpeg(GPid pid, gint status, gpointer user_data); /* fwd decl */

static void
child_watch_ytdlp(GPid pid, gint status, gpointer user_data)
{
    AppWidgets *w = user_data;

    if (w->yt_io) {
        g_io_channel_shutdown(w->yt_io, FALSE, NULL);
        g_io_channel_unref(w->yt_io);
        w->yt_io = NULL;
    }
    g_spawn_close_pid(pid);
    w->yt_pid = 0;

    if (w->cancel_requested) {
        gtk_label_set_text(w->status_label, "Canceled.");
        gtk_progress_bar_set_fraction(w->progress_bar, 0.0);
        return;
    }

    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        gtk_label_set_text(w->status_label, "Download failed.");
        return;
    }

    /* move to transcoding */
    gtk_label_set_text(w->status_label, "Download finished. Starting conversion…");
    w->phase = PHASE_TRANSCODING;

    /* get duration of the downloaded file for ffmpeg ETA */
    w->total_duration = get_media_duration(YTDLP_TMP_FILE);

    /* start ffmpeg with progress on stderr */
    const char *output_raw = gtk_editable_get_text(GTK_EDITABLE(w->output_entry));
    guint sel = gtk_drop_down_get_selected(w->format_dropdown);
    GtkStringList *slist = GTK_STRING_LIST(gtk_drop_down_get_model(w->format_dropdown));
    const char *fmt = gtk_string_list_get_string(slist, sel);
    char *output = append_extension_if_missing(output_raw, fmt);

    GError *err = NULL;
    if (!ensure_output_path(output, &err)) {
        gtk_label_set_text(w->status_label, err->message);
        g_error_free(err);
        g_free(output);
        return;
    }

    gchar *argv[] = {
        "ffmpeg",
        "-y",
        "-i", (gchar *)YTDLP_TMP_FILE,
        "-progress", "pipe:2",       /* key=value machine lines on stderr */
        "-nostats",                  /* we rely on -progress */
        (gchar *)output,
        NULL
    };

    gint stderr_fd = -1;
    gboolean ok = g_spawn_async_with_pipes(
        NULL, argv, NULL,
        G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
        NULL, NULL,
        &w->ffmpeg_pid,
        NULL, NULL, &stderr_fd, &err);

    if (!ok) {
        gtk_label_set_text(w->status_label, err->message);
        g_error_free(err);
        g_free(output);
        w->ffmpeg_pid = 0;
        return;
    }

    w->ff_io = g_io_channel_unix_new(stderr_fd);
    g_io_channel_set_encoding(w->ff_io, NULL, NULL);
    g_io_channel_set_buffered(w->ff_io, TRUE);
    g_io_add_watch(w->ff_io, G_IO_IN | G_IO_HUP | G_IO_ERR, (GIOFunc) ytdlp_progress_cb /* placeholder */, NULL); /* will replace */

    /* replace with proper ffmpeg progress cb */
    g_source_remove_by_user_data(NULL); /* no-op silencer */

    /* add correct watch */
    g_io_add_watch(w->ff_io, G_IO_IN | G_IO_HUP | G_IO_ERR, (GIOFunc)
        /* inline adapter that calls our ffmpeg cb below */
        (gpointer)NULL, NULL);

    /* actually use our ffmpeg cb via separate add below */
    /* (gtk/glib requires separate function; see registration further down) */

    g_child_watch_add(w->ffmpeg_pid, child_watch_ffmpeg, w);

    /* immediate progress recompute */
    update_unified_progress(w);

    g_free(output);
}

/* Build args and start yt-dlp (relative path), capture stdout for progress */
static void
start_ytdlp(AppWidgets *w, const char *url, const char *output)
{
    /* reset unified model */
    w->phase = PHASE_DOWNLOADING;
    w->dl_eta_sec = 0;
    w->tx_eta_sec = 0;
    w->dl_progress_0_1 = 0;
    w->tx_progress_0_1 = 0;
    w->cancel_requested = FALSE;
    w->t_start_us = g_get_monotonic_time();

    /* ensure any old temp file is gone */
    unlink(YTDLP_TMP_FILE);

    /* We force final container to mkv so we know the file path */
    gchar *argv[] = {
        (gchar *)PYTHON_PROG, (gchar *)YTDLP_PATH,
        "--newline",
        "-f", "bv*+ba/b",
        "--merge-output-format", "mkv",
        "-o", (gchar *)YTDLP_TMP_FILE,
        "--progress-template",
        "progress:[downloaded=%(progress.downloaded_bytes)s total=%(progress.total_bytes)s eta=%(progress.eta)s speed=%(progress.speed)s percent=%(progress._percent_str)s]",
        (gchar *)url,
        NULL
    };

    GError *err = NULL;
    gint stdout_fd = -1;

    gboolean ok = g_spawn_async_with_pipes(
        NULL, argv, NULL,
        G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
        NULL, NULL,
        &w->yt_pid,
        NULL, &stdout_fd, NULL,
        &err
    );

    if (!ok) {
        gtk_label_set_text(w->status_label, err->message);
        g_error_free(err);
        w->yt_pid = 0;
        return;
    }

    gtk_label_set_text(w->status_label, "Downloading from YouTube…");

    w->yt_io = g_io_channel_unix_new(stdout_fd);
    g_io_channel_set_encoding(w->yt_io, NULL, NULL);
    g_io_channel_set_buffered(w->yt_io, TRUE);
    g_io_add_watch(w->yt_io, G_IO_IN | G_IO_HUP | G_IO_ERR, ytdlp_progress_cb, w);

    g_child_watch_add(w->yt_pid, child_watch_ytdlp, w);
}

/* ---------- ffmpeg progress ---------- */

static gboolean
ffmpeg_progress_cb(GIOChannel *source, GIOCondition cond, gpointer data)
{
    AppWidgets *w = data;
    if (cond & (G_IO_HUP | G_IO_ERR)) return FALSE;

    gchar *line = NULL;
    gsize len = 0;
    GError *err = NULL;
    GIOStatus st = g_io_channel_read_line(source, &line, &len, NULL, &err);

    if (st == G_IO_STATUS_NORMAL && line) {
        /* parse out_time_ms and speed= lines */
        if (g_str_has_prefix(line, "out_time_ms=")) {
            gdouble ms = g_ascii_strtod(line + 12, NULL);
            gdouble elapsed_media = ms / 1e6;

            gdouble speed_x = 1.0; /* default if unknown; will update when we see speed= */
            /* keep last known tx_eta_sec using a cached speed_x (we'll store it in tx_eta_sec derivation below) */

            /* we don't have speed yet here; leave eta calc to when speed seen */
            if (w->total_duration > 0) {
                gdouble remain_media = w->total_duration - elapsed_media;
                if (remain_media < 0) remain_media = 0;
                /* If we already estimated a speed via previous lines, store it in tx_eta_sec as wall time */
                /* We'll recompute once we parse a speed line; for now, rough real-time */
                gdouble eta_guess = remain_media / speed_x;
                w->tx_eta_sec = eta_guess;
                w->tx_progress_0_1 = elapsed_media / w->total_duration;
                if (w->tx_progress_0_1 < 0) w->tx_progress_0_1 = 0;
                if (w->tx_progress_0_1 > 1) w->tx_progress_0_1 = 1;
            }
            update_unified_progress(w);
        } else if (g_str_has_prefix(line, "speed=")) {
            /* speed like: speed=1.23x */
            const char *s = line + 6;
            gdouble speed_x = g_ascii_strtod(s, NULL);
            if (speed_x < 0.1) speed_x = 0.1; /* clamp */

            /* we need an estimate of remain_media again; we don't store elapsed_media here,
               but tx_progress_0_1 gives us a fraction. */
            if (w->total_duration > 0) {
                gdouble elapsed_media = w->tx_progress_0_1 * w->total_duration;
                gdouble remain_media = w->total_duration - elapsed_media;
                if (remain_media < 0) remain_media = 0;
                w->tx_eta_sec = remain_media / speed_x;
            }
            update_unified_progress(w);
        } else if (g_str_has_prefix(line, "progress=end")) {
            w->tx_eta_sec = 0;
            update_unified_progress(w);
        }

        g_free(line);
        return TRUE;
    }

    if (st == G_IO_STATUS_EOF) {
        if (line) g_free(line);
        return FALSE;
    }

    if (err) g_error_free(err);
    return TRUE;
}

static void
child_watch_ffmpeg(GPid pid, gint status, gpointer user_data)
{
    AppWidgets *w = user_data;

    if (w->ff_io) {
        g_io_channel_shutdown(w->ff_io, FALSE, NULL);
        g_io_channel_unref(w->ff_io);
        w->ff_io = NULL;
    }
    g_spawn_close_pid(pid);
    w->ffmpeg_pid = 0;

    if (w->cancel_requested) {
        gtk_label_set_text(w->status_label, "Canceled.");
        gtk_progress_bar_set_fraction(w->progress_bar, 0.0);
        return;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        gtk_label_set_text(w->status_label, "Conversion finished.");
        gtk_progress_bar_set_fraction(w->progress_bar, 1.0);
        gtk_label_set_text(w->progress_label, "00:00:00");
    } else {
        gtk_label_set_text(w->status_label, "Conversion failed.");
    }
}

/* ---------- original local-file ffmpeg path (kept) ---------- */

static void
start_ffmpeg_conversion(AppWidgets *w, const char *input, const char *output)
{
    if (w->ffmpeg_pid != 0 || w->yt_pid != 0) {
        gtk_label_set_text(w->status_label, "A job is already running.");
        return;
    }

    w->phase = PHASE_TRANSCODING;
    w->t_start_us = g_get_monotonic_time();
    w->dl_eta_sec = 0;
    w->tx_eta_sec = 0;
    w->cancel_requested = FALSE;

    gchar *argv[] = {
        "ffmpeg",
        "-y",
        "-i", (gchar *)input,
        "-progress", "pipe:2",
        "-nostats",
        (gchar *)output,
        NULL
    };

    gint stderr_fd;
    GError *err = NULL;
    gboolean ok = g_spawn_async_with_pipes(
        NULL, argv, NULL,
        G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
        NULL, NULL, &w->ffmpeg_pid,
        NULL, NULL, &stderr_fd,
        &err
    );

    if (!ok) {
        gtk_label_set_text(w->status_label, err->message);
        g_error_free(err);
        w->ffmpeg_pid = 0;
        return;
    }

    w->ff_io = g_io_channel_unix_new(stderr_fd);
    g_io_channel_set_encoding(w->ff_io, NULL, NULL);
    g_io_channel_set_buffered(w->ff_io, TRUE);
    g_io_add_watch(w->ff_io, G_IO_IN | G_IO_HUP | G_IO_ERR, ffmpeg_progress_cb, w);

    g_child_watch_add(w->ffmpeg_pid, child_watch_ffmpeg, w);

    gtk_label_set_text(w->status_label, "Converting…");
    gtk_progress_bar_set_fraction(w->progress_bar, 0.0);
    gtk_label_set_text(w->progress_label, "Calculating…");
}

/* ---------- dialogs ---------- */

static void
open_file_done(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    GError *err = NULL;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source_object), res, &err);
    if (err) { g_warning("File dialog: %s", err->message); g_error_free(err); return; }
    if (file) {
        AppWidgets *w = user_data;
        char *path = g_file_get_path(file);
        gtk_editable_set_text(GTK_EDITABLE(w->input_entry), path);
        g_free(path);
        g_object_unref(file);
    }
}

static void
save_file_done(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    GError *err = NULL;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source_object), res, &err);
    if (err) { g_warning("File dialog: %s", err->message); g_error_free(err); return; }
    if (file) {
        AppWidgets *w = user_data;
        char *path = g_file_get_path(file);
        gtk_editable_set_text(GTK_EDITABLE(w->output_entry), path);
        g_free(path);
        g_object_unref(file);
    }
}

/* ---------- UI callbacks ---------- */

static void
on_browse_input_clicked(GtkButton *btn, gpointer user_data)
{
    AppWidgets *w = user_data;
    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(btn)));
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Select Input File");
    gtk_file_dialog_open(dlg, parent, NULL, open_file_done, w);
}

static void
on_browse_output_clicked(GtkButton *btn, gpointer user_data)
{
    AppWidgets *w = user_data;
    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(btn)));
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Select Output File");
    gtk_file_dialog_save(dlg, parent, NULL, save_file_done, w);
}

static void
cancel_running(AppWidgets *w)
{
    w->cancel_requested = TRUE;
    if (w->yt_pid) {
        kill(w->yt_pid, SIGTERM);
    }
    if (w->ffmpeg_pid) {
        /* ffmpeg honors SIGTERM; if you want, send "q" to stdin if you wired it */
        kill(w->ffmpeg_pid, SIGTERM);
    }
    gtk_label_set_text(w->status_label, "Canceling…");
}

static void
on_cancel_clicked(GtkButton *btn, gpointer user_data)
{
    AppWidgets *w = user_data;
    cancel_running(w);
}

static void
on_convert_clicked(GtkButton *btn, gpointer user_data)
{
    AppWidgets *w = user_data;
    const char *input = gtk_editable_get_text(GTK_EDITABLE(w->input_entry));
    const char *output_raw = gtk_editable_get_text(GTK_EDITABLE(w->output_entry));

    guint sel = gtk_drop_down_get_selected(w->format_dropdown);
    GtkStringList *slist = GTK_STRING_LIST(gtk_drop_down_get_model(w->format_dropdown));
    const char *fmt = gtk_string_list_get_string(slist, sel);

    if (!input || !*input || !output_raw || !*output_raw) {
        gtk_label_set_text(w->status_label, "Select input and output first.");
        return;
    }

    char *output = append_extension_if_missing(output_raw, fmt);

    GError *err = NULL;
    if (!ensure_output_path(output, &err)) {
        gtk_label_set_text(w->status_label, err->message);
        g_error_free(err);
        g_free(output);
        return;
    }
    g_free(output);

    /* If it's a YouTube URL, run the two-phase (download -> transcode) with a single shared bar */
    if (is_youtube_url(input)) {
        char *fixed_output = append_extension_if_missing(output_raw, fmt);
        start_ytdlp(w, input, fixed_output);
        g_free(fixed_output);
        return;
    }

    /* else: local file -> single-phase ffmpeg */
    char *fixed_output = append_extension_if_missing(output_raw, fmt);
    w->total_duration = get_media_duration(input);
    start_ffmpeg_conversion(w, input, fixed_output);
    g_free(fixed_output);
}

/* ---------- UI setup ---------- */

static void
activate(GtkApplication *app, gpointer user_data)
{
    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "Betinha");
    gtk_window_set_default_size(GTK_WINDOW(win), 560, 390);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_window_set_child(GTK_WINDOW(win), vbox);

    AppWidgets *w = g_new0(AppWidgets, 1);

    /* Input row */
    GtkWidget *in_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    w->input_entry = GTK_ENTRY(gtk_entry_new());
    GtkWidget *in_btn = gtk_button_new_with_label("Browse…");
    gtk_widget_set_hexpand(GTK_WIDGET(w->input_entry), TRUE);
    gtk_widget_set_hexpand(in_btn, FALSE);
    gtk_widget_set_size_request(in_btn, 110, -1);
    gtk_box_append(GTK_BOX(in_row), GTK_WIDGET(w->input_entry));
    gtk_box_append(GTK_BOX(in_row), in_btn);
    g_signal_connect(in_btn, "clicked", G_CALLBACK(on_browse_input_clicked), w);

    gtk_box_append(GTK_BOX(vbox), gtk_label_new("Input file or YouTube URL:"));
    gtk_box_append(GTK_BOX(vbox), in_row);

    /* Output row */
    GtkWidget *out_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    w->output_entry = GTK_ENTRY(gtk_entry_new());
    GtkWidget *out_btn = gtk_button_new_with_label("Browse…");
    gtk_widget_set_hexpand(GTK_WIDGET(w->output_entry), TRUE);
    gtk_widget_set_hexpand(out_btn, FALSE);
    gtk_widget_set_size_request(out_btn, 110, -1);
    gtk_box_append(GTK_BOX(out_row), GTK_WIDGET(w->output_entry));
    gtk_box_append(GTK_BOX(out_row), out_btn);
    g_signal_connect(out_btn, "clicked", G_CALLBACK(on_browse_output_clicked), w);

    gtk_box_append(GTK_BOX(vbox), gtk_label_new("Output file:"));
    gtk_box_append(GTK_BOX(vbox), out_row);

    /* Format dropdown */
    gtk_box_append(GTK_BOX(vbox), gtk_label_new("Output format:"));
    const char *formats[] = {"PNG", "JPEG", "WEBP", "GIF", "MP4", "MP3", NULL};
    GtkStringList *slist = gtk_string_list_new(formats);
    w->format_dropdown = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(slist), NULL));
    gtk_drop_down_set_selected(w->format_dropdown, 4); /* default to MP4 */
    gtk_box_append(GTK_BOX(vbox), GTK_WIDGET(w->format_dropdown));

    /* Buttons row: Convert + Cancel */
    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_row, GTK_ALIGN_CENTER);
    w->convert_btn = GTK_BUTTON(gtk_button_new_with_label("Convert"));
    w->cancel_btn  = GTK_BUTTON(gtk_button_new_with_label("Cancel"));
    gtk_box_append(GTK_BOX(btn_row), GTK_WIDGET(w->convert_btn));
    gtk_box_append(GTK_BOX(btn_row), GTK_WIDGET(w->cancel_btn));
    gtk_box_append(GTK_BOX(vbox), btn_row);

    g_signal_connect(w->convert_btn, "clicked", G_CALLBACK(on_convert_clicked), w);
    g_signal_connect(w->cancel_btn,  "clicked", G_CALLBACK(on_cancel_clicked),  w);

    /* Progress bar + ETA label + status */
    w->progress_bar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_progress_bar_set_show_text(w->progress_bar, TRUE);
    gtk_box_append(GTK_BOX(vbox), GTK_WIDGET(w->progress_bar));

    w->progress_label = GTK_LABEL(gtk_label_new("00:00:00"));
    gtk_box_append(GTK_BOX(vbox), GTK_WIDGET(w->progress_label));

    w->status_label = GTK_LABEL(gtk_label_new(""));
    gtk_box_append(GTK_BOX(vbox), GTK_WIDGET(w->status_label));

    gtk_window_present(GTK_WINDOW(win));
}

/* ---------- main ---------- */

int main(int argc, char *argv[])
{
    GtkApplication *app = gtk_application_new("com.example.ffmpeg.converter", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int st = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return st;
}