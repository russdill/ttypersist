AC_INIT([ttypersist],[1.0],[Russ.Dill@gmail.com])

AC_PREREQ([2.61])
AC_CONFIG_AUX_DIR([config])

AC_CONFIG_SRCDIR([ttypersist.c])

AC_CHECK_LIB(pthread, pthread_create)
AC_CHECK_LIB(dl, dlsym)

LT_PREREQ([2.2])
LT_INIT([disable-static])

AM_INIT_AUTOMAKE([foreign])

AC_OUTPUT([Makefile])

