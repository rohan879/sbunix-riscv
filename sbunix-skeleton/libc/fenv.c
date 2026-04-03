#include <fenv.h>

static fexcept_t g_excepts;
static int g_round = FE_TONEAREST;

int feclearexcept(int excepts) {
  g_excepts &= ~(fexcept_t)excepts;
  return 0;
}

int fegetexceptflag(fexcept_t *flagp, int excepts) {
  if (!flagp)
    return -1;
  *flagp = g_excepts & (fexcept_t)excepts;
  return 0;
}

int feraiseexcept(int excepts) {
  g_excepts |= (fexcept_t)excepts;
  return 0;
}

int fesetexceptflag(const fexcept_t *flagp, int excepts) {
  if (!flagp)
    return -1;
  g_excepts = (g_excepts & ~(fexcept_t)excepts) | (*flagp & (fexcept_t)excepts);
  return 0;
}

int fetestexcept(int excepts) {
  return (int)(g_excepts & (fexcept_t)excepts);
}

int fegetround(void) {
  return g_round;
}

int fesetround(int round) {
  if (round != FE_TONEAREST && round != FE_DOWNWARD &&
      round != FE_UPWARD && round != FE_TOWARDZERO)
    return -1;
  g_round = round;
  return 0;
}

int fegetenv(fenv_t *envp) {
  if (!envp)
    return -1;
  envp->__excepts = g_excepts;
  envp->__round = g_round;
  return 0;
}

int feholdexcept(fenv_t *envp) {
  if (!envp)
    return -1;
  envp->__excepts = g_excepts;
  envp->__round = g_round;
  g_excepts = 0;
  return 0;
}

int fesetenv(const fenv_t *envp) {
  if (envp == FE_DFL_ENV) {
    g_excepts = 0;
    g_round = FE_TONEAREST;
    return 0;
  }
  if (!envp)
    return -1;
  g_excepts = envp->__excepts;
  g_round = envp->__round;
  return 0;
}

int feupdateenv(const fenv_t *envp) {
  fexcept_t raised = g_excepts;
  if (fesetenv(envp) < 0)
    return -1;
  g_excepts |= raised;
  return 0;
}
