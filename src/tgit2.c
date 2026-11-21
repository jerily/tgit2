#include <git2.h>
#include <tcl.h>
#include <string.h>

/* libgit2 expects NUL-terminated, standard UTF-8 on every platform. */
static int utf8_string(Tcl_Interp *interp, Tcl_Encoding utf8, Tcl_Obj *obj, Tcl_DString *buffer)
{
#if TCL_MAJOR_VERSION >= 9
    Tcl_Size length;
#else
    int length;
#endif
    const char *value = Tcl_GetStringFromObj(obj, &length);
    Tcl_UtfToExternalDString(utf8, value, length, buffer);
    if (memchr(Tcl_DStringValue(buffer), 0, Tcl_DStringLength(buffer))) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("unexpected NUL in clone argument", -1));
        return TCL_ERROR;
    }
    return TCL_OK;
}

static int git_failure(Tcl_Interp *interp)
{
    const git_error *error = git_error_last();
    Tcl_SetObjResult(interp, Tcl_ObjPrintf("libgit2: %s", error ? error->message : "clone failed"));
    Tcl_SetErrorCode(interp, "TGIT2", "CLONE", NULL);
    return TCL_ERROR;
}

static int clone_cmd(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    git_clone_options options = GIT_CLONE_OPTIONS_INIT;
    git_repository *repo = NULL;
    Tcl_Encoding utf8;
    Tcl_DString url, path;
    Tcl_Obj *destination;
    int result = TCL_ERROR;
    (void)data;

    if (objc == 5 && strcmp(Tcl_GetString(objv[1]), "-depth") == 0) {
        if (Tcl_GetIntFromObj(interp, objv[2], &options.fetch_opts.depth) != TCL_OK)
            return TCL_ERROR;
        if (options.fetch_opts.depth < 1) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("depth must be a positive integer", -1));
            return TCL_ERROR;
        }
        /* The local copy optimization would silently copy the full history. */
        options.local = GIT_CLONE_NO_LOCAL;
    } else if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "?-depth count? url directory");
        return TCL_ERROR;
    }
    destination = Tcl_FSGetNormalizedPath(interp, objv[objc - 1]);
    if (!destination) return TCL_ERROR;
    utf8 = Tcl_GetEncoding(interp, "utf-8");
    if (!utf8) return TCL_ERROR;
    Tcl_DStringInit(&url);
    Tcl_DStringInit(&path);
    if (utf8_string(interp, utf8, objv[objc - 2], &url) == TCL_OK &&
        utf8_string(interp, utf8, destination, &path) == TCL_OK) {
        if (git_libgit2_init() < 0) {
            git_failure(interp);
        } else {
            if (git_clone(&repo, Tcl_DStringValue(&url), Tcl_DStringValue(&path), &options) == 0)
                result = TCL_OK;
            else
                git_failure(interp);
            git_repository_free(repo);
            git_libgit2_shutdown();
        }
    }
    Tcl_DStringFree(&path);
    Tcl_DStringFree(&url);
    Tcl_FreeEncoding(utf8);
    return result;
}

DLLEXPORT int Tgit2_Init(Tcl_Interp *interp)
{
#if TCL_MAJOR_VERSION >= 9
    if (!Tcl_InitStubs(interp, "9.0", 0)) return TCL_ERROR;
#else
    if (!Tcl_InitStubs(interp, "8.6", 0)) return TCL_ERROR;
#endif
    if (!Tcl_CreateNamespace(interp, "::tgit2", NULL, NULL)) return TCL_ERROR;
    Tcl_CreateObjCommand(interp, "::tgit2::clone", clone_cmd, NULL, NULL);
    return Tcl_PkgProvide(interp, "tgit2", PACKAGE_VERSION);
}
