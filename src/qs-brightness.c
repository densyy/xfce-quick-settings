/* qs-brightness.c — backlight/brightness responsibility.
 *
 * TU própria (Makefile SRC); compartilha a fachada qs-display.h com
 * qs-display.c (seção) / qs-nightlight.c / qs-monitor.c.
 *
 * Perf:
 * - display_refresh_brightness nunca faz spawn síncrono na UI: backend
 *   sysfs lê arquivos (rápido, sem spawn) direto; brightnessctl/xbacklight/
 *   xrandr vão via run_command_async (worker thread, volta no main loop).
 *   Em regime só 1 spawn em background (antes até 4 sync na UI).
 * - Backend + device + output xrandr em cache (só PATH/readdir uma vez).
 * - on_brightness_changed com debounce (~70ms): arrastar o slider agenda
 *   um apply, não um spawn por tick. Setter nunca roda `xrandr --verbose`
 *   síncrono por tick (usa output em cache; miss vai via async).
 * - atoi substituído por g_ascii_strtoull/strtod com checagem.
 */
#include "qs-display.h"
#include "qs-ui.h"
#include "qs-util.h"

#include <errno.h>
#include <string.h>

/* Debounce do slider: um apply por rajada de "value-changed". */
#define BRIGHT_DEBOUNCE_MS 70

typedef enum
{
  BRIGHT_UNKNOWN = 0,
  BRIGHT_CTL,
  BRIGHT_SYSFS,
  BRIGHT_XBACK,
  BRIGHT_XRANDR,
  BRIGHT_NONE
} BrightBackend;

/* Cache (só tocado na main thread; worker só faz spawn, nunca lê isso). */
static BrightBackend bright_backend = BRIGHT_UNKNOWN;
static gchar *bright_dev = NULL;
static gchar *bright_xout = NULL;
static gchar *bright_ctl_bin = NULL;
static gboolean bright_has_ctl = FALSE;
static gboolean bright_has_xback = FALSE;
static gboolean bright_has_xrandr = FALSE;
static gboolean bright_tools_probed = FALSE;

static guint bright_timer_id = 0;
static gint bright_pending_val = -1;

static gboolean
qs_parse_int_checked (const gchar *s,
                      gint        *out)
{
  const gchar *p;
  gchar *end = NULL;
  guint64 v;

  if (s == NULL || out == NULL)
    return FALSE;
  p = s;
  while (g_ascii_isspace (*p))
    p++;
  if (*p == '\0')
    return FALSE;
  if (*p == '-')
    return FALSE;
  if (*p == '+')
    {
      p++;
      if (*p == '\0')
        return FALSE;
    }
  errno = 0;
  v = g_ascii_strtoull (p, &end, 10);
  if (errno != 0 || end == p)
    return FALSE;
  while (*end != '\0' && g_ascii_isspace (*end))
    end++;
  if (*end != '\0')
    return FALSE;
  if (v > (guint64) G_MAXINT)
    return FALSE;
  *out = (gint) v;
  return TRUE;
}

static gboolean
qs_parse_double_checked (const gchar *s,
                         gdouble     *out)
{
  const gchar *p;
  gchar *end = NULL;
  gdouble v;

  if (s == NULL || out == NULL)
    return FALSE;
  p = s;
  while (g_ascii_isspace (*p))
    p++;
  if (*p == '\0')
    return FALSE;
  errno = 0;
  v = g_ascii_strtod (p, &end);
  if (errno != 0 || end == p)
    return FALSE;
  while (*end != '\0' && g_ascii_isspace (*end))
    end++;
  if (*end != '\0')
    return FALSE;
  *out = v;
  return TRUE;
}

/* Primeiro "NN%" do texto (saída brightnessctl -m), ou -1. Versão validada
 * local: não depende de parse_first_percent (que usa atoi no qs-util). */
static gint
bright_parse_first_percent (const gchar *text)
{
  const gchar *pct;
  const gchar *start;
  gchar *num = NULL;
  gint value = -1;

  if (text == NULL)
    return -1;
  pct = strchr (text, '%');
  if (pct == NULL)
    return -1;
  start = pct;
  while (start > text && g_ascii_isdigit (*(start - 1)))
    start--;
  if (start == pct)
    return -1;
  num = g_strndup (start, (gsize) (pct - start));
  if (!qs_parse_int_checked (num, &value))
    value = -1;
  g_free (num);
  return value;
}

/* Nome de device seguro para interpolar sem quote ([A-Za-z0-9_+-]). */
static gboolean
bright_valid_dev (const gchar *s)
{
  const gchar *p;

  if (s == NULL || *s == '\0' || strlen (s) > 64)
    return FALSE;
  for (p = s; *p != '\0'; p++)
    if (!g_ascii_isalnum (*p) && *p != '_' && *p != '+' && *p != '-')
      return FALSE;
  return TRUE;
}

/* Melhor device em /sys/class/backlight, ou NULL (caller frees).
 * Descarta max<=0 ou ilegível; prefere nativo a acpi_video*. */
static gchar *
pick_backlight_device (void)
{
  GDir *dir = g_dir_open ("/sys/class/backlight", 0, NULL);
  const gchar *e;
  gchar *best = NULL;
  gint best_score = -1;

  if (dir == NULL)
    return NULL;
  while ((e = g_dir_read_name (dir)) != NULL)
    {
      gchar *mp = g_build_filename ("/sys/class/backlight", e,
                                    "max_brightness", NULL);
      gchar *c = NULL;
      gint maxv = 0;
      gboolean ok = FALSE;
      gint score;

      if (g_file_get_contents (mp, &c, NULL, NULL))
        ok = qs_parse_int_checked (c, &maxv);
      g_free (c);
      g_free (mp);
      if (!ok || maxv <= 0)
        continue;
      if (g_str_has_prefix (e, "intel_backlight")
          || strstr (e, "amdgpu") != NULL
          || strstr (e, "nvidia") != NULL
          || g_str_has_prefix (e, "nv_"))
        score = 2;
      else if (g_str_has_prefix (e, "acpi_video"))
        score = 0;
      else
        score = 1;
      if (score > best_score)
        {
          g_free (best);
          best = g_strdup (e);
          best_score = score;
        }
    }
  g_dir_close (dir);
  return best;
}

static void
bright_probe_tools (void)
{
  if (bright_tools_probed)
    return;
  bright_has_ctl = command_exists ("brightnessctl");
  bright_has_xback = command_exists ("xbacklight");
  bright_has_xrandr = command_exists ("xrandr");
  bright_tools_probed = TRUE;
  if (bright_has_ctl && bright_ctl_bin == NULL)
    bright_ctl_bin = g_find_program_in_path ("brightnessctl");
  if (!bright_has_ctl)
    {
      g_free (bright_ctl_bin);
      bright_ctl_bin = NULL;
    }
}

/* Device em cache (pick uma vez). Borrowed, pode ser NULL. */
static const gchar *
bright_ensure_dev (void)
{
  if (bright_dev == NULL)
    bright_dev = pick_backlight_device ();
  if (!bright_valid_dev (bright_dev))
    {
      g_free (bright_dev);
      bright_dev = NULL;
    }
  return bright_dev;
}

/* Backend barato (sem spawn): PATH + readdir uma vez, depois cache. */
static BrightBackend
bright_probe_backend (void)
{
  const gchar *dev;

  if (bright_backend != BRIGHT_UNKNOWN)
    return bright_backend;
  bright_probe_tools ();
  if (bright_has_ctl)
    {
      bright_backend = BRIGHT_CTL;
      (void) bright_ensure_dev (); /* para o -d */
      return bright_backend;
    }
  dev = bright_ensure_dev ();
  if (dev != NULL)
    {
      bright_backend = BRIGHT_SYSFS;
      return bright_backend;
    }
  if (bright_has_xback)
    {
      bright_backend = BRIGHT_XBACK;
      return bright_backend;
    }
  if (bright_has_xrandr)
    {
      bright_backend = BRIGHT_XRANDR;
      return bright_backend;
    }
  bright_backend = BRIGHT_NONE;
  return bright_backend;
}

/* Invalida backend (força re-probe barato na próxima vez). Usado quando o
 * backend em cache falha (binário removido, sysfs sumiu). */
static void
bright_invalidate_backend (void)
{
  bright_backend = BRIGHT_UNKNOWN;
  bright_tools_probed = FALSE;
  g_free (bright_ctl_bin);
  bright_ctl_bin = NULL;
}

/* Leitura sysfs síncrona (rápida, sem spawn). -1 se indisponível. */
static gint
bright_read_sysfs (void)
{
  const gchar *dev = bright_ensure_dev ();
  gchar *bright_path = NULL;
  gchar *max_path = NULL;
  gchar *contents = NULL;
  gint brightness = 0;
  gint max_brightness = 0;
  gint value = -1;

  if (dev == NULL)
    return -1;
  bright_path = g_build_filename ("/sys/class/backlight", dev,
                                  "brightness", NULL);
  max_path = g_build_filename ("/sys/class/backlight", dev,
                               "max_brightness", NULL);
  if (g_file_get_contents (bright_path, &contents, NULL, NULL)
      && qs_parse_int_checked (contents, &brightness))
    {
      g_free (contents);
      contents = NULL;
      if (g_file_get_contents (max_path, &contents, NULL, NULL)
          && qs_parse_int_checked (contents, &max_brightness)
          && max_brightness > 0 && brightness >= 0)
        value = (gint) (((gint64) brightness * 100) / max_brightness);
    }
  g_free (contents);
  g_free (bright_path);
  g_free (max_path);
  if (value < 0)
    {
      /* Device sumiu/ilegível: limpa para re-pick na próxima vez. */
      g_free (bright_dev);
      bright_dev = NULL;
    }
  return value;
}

/* Primeiro output " connected" de um dump xrandr (para cache do setter).
 * Caller frees. */
static gchar *
bright_extract_output (const gchar *dump)
{
  gchar **lines;
  gint i;
  gchar *name = NULL;

  if (dump == NULL)
    return NULL;
  lines = g_strsplit (dump, "\n", -1);
  for (i = 0; lines[i] != NULL && name == NULL; i++)
    {
      if (strstr (lines[i], " connected") != NULL)
        {
          gchar **tok = g_strsplit (lines[i], " ", 2);

          if (tok[0] != NULL && valid_monitor_shared (tok[0]))
            name = g_strdup (tok[0]);
          g_strfreev (tok);
        }
    }
  g_strfreev (lines);
  return name;
}

static void
bright_apply_to_slider (QuickSettings *qs,
                        gint           value)
{
  if (qs->dead || qs->bright_scale == NULL)
    return;
  qs->updating = TRUE;
  /* Se o sistema está abaixo do piso (ex: zerado por fora), mostra o
   * piso: mover o slider já recupera para >= MIN. */
  gtk_range_set_value (GTK_RANGE (qs->bright_scale),
                       CLAMP (value, BRIGHTNESS_MIN, 100));
  gtk_widget_set_sensitive (qs->bright_scale, TRUE);
  gtk_widget_set_tooltip_text (qs->bright_scale, "Screen brightness");
  qs->updating = FALSE;
}

static void
bright_disable_slider (QuickSettings *qs)
{
  if (qs->dead || qs->bright_scale == NULL)
    return;
  qs->updating = TRUE;
  gtk_widget_set_sensitive (qs->bright_scale, FALSE);
  gtk_widget_set_tooltip_text (qs->bright_scale, "Brightness unavailable");
  qs->updating = FALSE;
}

/* Callbacks async (main loop, com ref do qs). Cada um consome a ref no fim
 * da cadeia; intermediários repassam a mesma ref sem unref extra. */

static void
bright_on_xrandr (const gchar *output,
                  const gchar *errout,
                  gboolean     ok,
                  gpointer     user_data);

static void
bright_on_xback (const gchar *output,
                 const gchar *errout,
                 gboolean     ok,
                 gpointer     user_data);

static void
bright_on_ctl2 (const gchar *output,
                const gchar *errout,
                gboolean     ok,
                gpointer     user_data)
{
  QuickSettings *qs = user_data;
  gint value = -1;

  (void) errout;
  if (qs->dead)
    {
      qs_unref (qs);
      return;
    }
  if (ok && output != NULL)
    value = bright_parse_first_percent (output);
  if (value >= 0)
    {
      bright_apply_to_slider (qs, value);
      qs_unref (qs);
      return;
    }
  bright_invalidate_backend ();
  value = bright_read_sysfs ();
  if (value >= 0)
    {
      bright_probe_backend ();
      bright_apply_to_slider (qs, value);
      qs_unref (qs);
      return;
    }
  bright_probe_tools ();
  if (bright_has_xback)
    {
      run_command_async ("xbacklight -get", bright_on_xback, qs);
      return;
    }
  if (bright_has_xrandr)
    {
      run_command_async ("xrandr --verbose", bright_on_xrandr, qs);
      return;
    }
  bright_disable_slider (qs);
  qs_unref (qs);
}

static void
bright_on_ctl (const gchar *output,
               const gchar *errout,
               gboolean     ok,
               gpointer     user_data)
{
  QuickSettings *qs = user_data;
  gint value = -1;

  (void) errout;
  if (qs->dead)
    {
      qs_unref (qs);
      return;
    }
  if (ok && output != NULL)
    value = bright_parse_first_percent (output);
  if (value >= 0)
    {
      bright_apply_to_slider (qs, value);
      qs_unref (qs);
      return;
    }
  /* Tenta sem -c (brightnessctl antigo sem flag -c), mesma ref. */
  run_command_async ("brightnessctl -m", bright_on_ctl2, qs);
}

static void
bright_on_xback (const gchar *output,
                 const gchar *errout,
                 gboolean     ok,
                 gpointer     user_data)
{
  QuickSettings *qs = user_data;
  gint value = -1;

  (void) errout;
  if (qs->dead)
    {
      qs_unref (qs);
      return;
    }
  if (ok && output != NULL)
    {
      gdouble d = 0;

      if (qs_parse_double_checked (output, &d))
        value = (gint) d;
    }
  if (value >= 0)
    {
      bright_apply_to_slider (qs, CLAMP (value, 0, 100));
      qs_unref (qs);
      return;
    }
  bright_probe_tools ();
  if (bright_has_xrandr)
    {
      run_command_async ("xrandr --verbose", bright_on_xrandr, qs);
      return;
    }
  bright_disable_slider (qs);
  qs_unref (qs);
}

static void
bright_on_xrandr (const gchar *output,
                  const gchar *errout,
                  gboolean     ok,
                  gpointer     user_data)
{
  QuickSettings *qs = user_data;
  gint value = -1;

  (void) errout;
  if (qs->dead)
    {
      qs_unref (qs);
      return;
    }
  if (ok && output != NULL)
    {
      const gchar *p = strstr (output, "Brightness:");

      if (p != NULL)
        {
          gdouble d = 0;

          if (qs_parse_double_checked (p + 11, &d))
            value = (gint) (d * 100.0 + 0.5);
        }
      /* Aproveita o mesmo dump para aquecer o cache do setter. */
      if (bright_xout == NULL)
        bright_xout = bright_extract_output (output);
    }
  if (value >= 0)
    bright_apply_to_slider (qs, value);
  else
    bright_disable_slider (qs);
  qs_unref (qs);
}

/* Setter rápido (sem spawn síncrono): usa backend/device/output em cache e
 * só dispara spawn_command (async). Miss de output xrandr resolve via
 * async, nunca bloqueia o tick do slider. */
typedef struct
{
  QuickSettings *qs;
  gint           value;
} BrightSet;

static void
bright_set_output_done (const gchar *output,
                        const gchar *errout,
                        gboolean     ok,
                        gpointer     user_data)
{
  BrightSet *b = user_data;
  QuickSettings *qs = b->qs;

  (void) errout;
  if (!qs->dead && ok && output != NULL)
    {
      gchar *name = bright_extract_output (output);

      if (name != NULL)
        {
          gchar *qname;
          gchar *cmd;

          g_free (bright_xout);
          bright_xout = name;
          qname = g_shell_quote (bright_xout);
          cmd = g_strdup_printf ("xrandr --output %s --brightness %.2f",
                                 qname, b->value / 100.0);
          g_free (qname);
          spawn_command (cmd);
          g_free (cmd);
        }
    }
  qs_unref (qs);
  g_free (b);
}

static void
set_brightness_percent (gint value)
{
  BrightBackend be;
  const gchar *dev;

  value = CLAMP (value, BRIGHTNESS_MIN, 100);
  be = bright_probe_backend ();

  if (be == BRIGHT_CTL)
    {
      gchar *cmd;

      bright_probe_tools ();
      dev = bright_ensure_dev ();
      if (dev != NULL && bright_ctl_bin != NULL && bright_valid_dev (dev))
        {
          /* Nomes de device são [A-Za-z0-9_+-]: sem quote, sem risco de
           * parse do g_spawn_command_line_async. Caminho absoluto: o PATH
           * do painel pode ser mínimo. */
          cmd = g_strdup_printf ("%s -d %s set %d%%",
                                 bright_ctl_bin, dev, value);
        }
      else if (bright_ctl_bin != NULL)
        {
          cmd = g_strdup_printf ("%s set %d%%", bright_ctl_bin, value);
        }
      else
        {
          cmd = g_strdup_printf ("brightnessctl set %d%%", value);
        }
      spawn_command (cmd);
      g_free (cmd);
      return;
    }

  dev = bright_ensure_dev ();
  if (dev != NULL)
    {
      gchar *max_path = g_build_filename ("/sys/class/backlight", dev,
                                          "max_brightness", NULL);
      gchar *contents = NULL;
      gint max_brightness = 0;

      if (g_file_get_contents (max_path, &contents, NULL, NULL)
          && qs_parse_int_checked (contents, &max_brightness)
          && max_brightness > 0)
        {
          gchar *bright_path = g_build_filename ("/sys/class/backlight",
                                                 dev, "brightness", NULL);
          gchar *cmd = g_strdup_printf ("%d",
                                        (gint) (((gint64) value
                                                 * max_brightness) / 100));
          /* Direct sysfs write needs root: only claim handled on
           * success, otherwise fall through to xbacklight/xrandr. */
          if (g_file_set_contents (bright_path, cmd, -1, NULL))
            {
              g_free (cmd);
              g_free (bright_path);
              g_free (contents);
              g_free (max_path);
              return;
            }
          g_free (cmd);
          g_free (bright_path);
        }
      g_free (contents);
      g_free (max_path);
      bright_probe_tools ();
      if (bright_has_xback)
        {
          gchar *cmd = g_strdup_printf ("xbacklight -set %d", value);

          spawn_command (cmd);
          g_free (cmd);
          return;
        }
    }
  else
    {
      bright_probe_tools ();
      if (bright_has_xback)
        {
          gchar *cmd = g_strdup_printf ("xbacklight -set %d", value);

          spawn_command (cmd);
          g_free (cmd);
          return;
        }
    }

  bright_probe_tools ();
  if (bright_has_xrandr && dev == NULL)
    {
      /* Sem backlight real (desktop): gamma é a única opção.
       * Piso alto: 0.05 parece apagado e assusta. */
      gint vout = MAX (value, BRIGHTNESS_MIN_XRANDR);

      if (bright_xout != NULL && valid_monitor_shared (bright_xout))
        {
          gchar *qname = g_shell_quote (bright_xout);
          gchar *cmd = g_strdup_printf ("xrandr --output %s --brightness %.2f",
                                        qname, vout / 100.0);

          g_free (qname);
          spawn_command (cmd);
          g_free (cmd);
        }
      else
        {
          /* Miss de cache sem qs: uma query síncrona única de warmup
           * (só no primeiro tick em desktop sem backlight; os seguintes
           * usam cache). --query é mais leve que --verbose. */
          {
            gchar *dump = NULL;

            dump = run_command ("xrandr --query");
            if (dump != NULL)
              {
                gchar *name = bright_extract_output (dump);

                if (name != NULL)
                  {
                    gchar *qname = g_shell_quote (name);
                    gchar *cmd = g_strdup_printf ("xrandr --output %s "
                                                  "--brightness %.2f",
                                                  qname, vout / 100.0);

                    g_free (qname);
                    spawn_command (cmd);
                    g_free (cmd);
                    g_free (bright_xout);
                    bright_xout = name;
                  }
                g_free (dump);
              }
          }
        }
    }
}

/* Setter com qs (para o debounce poder resolver output via async com ref).
 * Caminho XRANDR com miss usa run_command_async com a ref do qs. */
static void
bright_set_for_qs (QuickSettings *qs,
                   gint           value)
{
  BrightBackend be;

  value = CLAMP (value, BRIGHTNESS_MIN, 100);
  be = bright_probe_backend ();

  if (be == BRIGHT_XRANDR || (be != BRIGHT_CTL && be != BRIGHT_SYSFS
                              && be != BRIGHT_XBACK))
    {
      const gchar *dev = bright_ensure_dev ();

      bright_probe_tools ();
      if (dev == NULL && bright_has_xrandr)
        {
          gint vout = MAX (value, BRIGHTNESS_MIN_XRANDR);

          if (bright_xout != NULL && valid_monitor_shared (bright_xout))
            {
              gchar *qname = g_shell_quote (bright_xout);
              gchar *cmd = g_strdup_printf ("xrandr --output %s "
                                            "--brightness %.2f",
                                            qname, vout / 100.0);

              g_free (qname);
              spawn_command (cmd);
              g_free (cmd);
              return;
            }
          if (bright_has_xrandr)
            {
              BrightSet *b = g_new0 (BrightSet, 1);

              b->value = vout;
              b->qs = qs;
              qs_ref (qs);
              run_command_async ("xrandr --verbose",
                                 bright_set_output_done, b);
              return;
            }
        }
    }
  set_brightness_percent (value);
}

static gboolean
bright_debounce_fire (gpointer user_data)
{
  QuickSettings *qs = user_data;
  gint v;

  bright_timer_id = 0;
  v = bright_pending_val;
  bright_pending_val = -1;
  if (!qs->dead && v >= 0)
    bright_set_for_qs (qs, v);
  qs_unref (qs);
  return G_SOURCE_REMOVE;
}

void
on_brightness_changed (GtkRange *range,
                       gpointer  user_data)
{
  QuickSettings *qs = user_data;
  gint value;

  if (qs->updating)
    return;
  if (!gtk_widget_is_sensitive (GTK_WIDGET (range)))
    return;
  value = (gint) (gtk_range_get_value (range) + 0.5);
  bright_pending_val = value;
  if (bright_timer_id != 0)
    return;
  qs_ref (qs);
  bright_timer_id = g_timeout_add (BRIGHT_DEBOUNCE_MS,
                                   bright_debounce_fire, qs);
}

/* Fachada estável (qs-display.h): refresh sem spawn síncrono na UI. */
void
display_refresh_brightness (QuickSettings *qs)
{
  BrightBackend be;
  gint value;

  if (qs->dead || qs->bright_scale == NULL)
    return;
  be = bright_probe_backend ();

  if (be == BRIGHT_SYSFS)
    {
      /* sysfs: só leitura de arquivos, rápido o bastante na UI. */
      value = bright_read_sysfs ();
      if (value >= 0)
        {
          bright_apply_to_slider (qs, value);
        }
      else
        {
          /* Falha uma vez: invalida e tenta o próximo backend sem
           * recursão aberta (evita loop se o device reaparecer no
           * re-pick mas seguir ilegível). */
          bright_invalidate_backend ();
          be = bright_probe_backend ();
          if (be == BRIGHT_SYSFS)
            bright_disable_slider (qs);
          else
            display_refresh_brightness (qs);
        }
      return;
    }
  if (be == BRIGHT_CTL)
    {
      qs_ref (qs);
      /* -m multi-linha quando há 2 devices; parser pega o primeiro %. */
      run_command_async ("brightnessctl -c backlight -m",
                         bright_on_ctl, qs);
      return;
    }
  if (be == BRIGHT_XBACK)
    {
      qs_ref (qs);
      run_command_async ("xbacklight -get", bright_on_xback, qs);
      return;
    }
  if (be == BRIGHT_XRANDR)
    {
      qs_ref (qs);
      run_command_async ("xrandr --verbose", bright_on_xrandr, qs);
      return;
    }
  bright_disable_slider (qs);
}
