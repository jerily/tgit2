#include <git2.h>
#include <tcl.h>
#include <string.h>
#include <limits.h>

#if TCL_MAJOR_VERSION < 9
typedef int TgitSize;
#else
typedef Tcl_Size TgitSize;
#endif

/* Convert Tcl's internal representation before passing a string to libgit2. */
static int external_string(Tcl_Interp *interp, Tcl_Encoding utf8, Tcl_Obj *obj, Tcl_DString *buf)
{
    TgitSize length;
    const char *s = Tcl_GetStringFromObj(obj, &length);
    Tcl_UtfToExternalDString(utf8, s, length, buf);
    if (memchr(Tcl_DStringValue(buf), 0, Tcl_DStringLength(buf))) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("unexpected NUL in argument", -1));
        return -1;
    }
    return 0;
}

static int invalid(Tcl_Interp *interp, const char *message)
{
    Tcl_SetObjResult(interp, Tcl_NewStringObj(message, -1));
    return -1;
}

static void oid_result(Tcl_Interp *interp, const git_oid *id)
{
    char hex[GIT_OID_HEXSZ + 1];
    Tcl_SetObjResult(interp, Tcl_NewStringObj(git_oid_tostr(hex, sizeof(hex), id), -1));
}

static int parse_oid(Tcl_Interp *interp, git_oid *out, const char *hex)
{
    if (strlen(hex) != GIT_OID_HEXSZ || git_oid_fromstr(out, hex) < 0)
        return invalid(interp, "expected a full hexadecimal commit ID");
    return 0;
}

typedef struct {
    Tcl_Interp *interp;
    Tcl_Encoding utf8;
    git_repository *repo;
    Tcl_Obj *const *obj;
    const char *arg[4];
} Call;

static int create_bare(git_repository **out, const char *path)
{
    git_repository_init_options opts = GIT_REPOSITORY_INIT_OPTIONS_INIT;
    opts.flags = GIT_REPOSITORY_INIT_BARE | GIT_REPOSITORY_INIT_NO_REINIT;
    opts.initial_head = "main";
    opts.template_path = "";
    return git_repository_init_ext(out, path, &opts);
}

static int init_repo(Call *c)
{
    git_repository *bare = NULL;
    git_treebuilder *builder = NULL;
    git_tree *tree = NULL;
    git_signature *author = NULL;
    git_oid tid, id;
    int rc = -1;

    if (Tcl_FSCreateDirectory(c->obj[0]) != TCL_OK)
        return invalid(c->interp, "could not create a fresh repository directory");
    if (create_bare(&bare, c->arg[0]) == 0 &&
        git_treebuilder_new(&builder, bare, NULL) == 0 &&
        git_treebuilder_write(&tid, builder) == 0 &&
        git_tree_lookup(&tree, bare, &tid) == 0 &&
        git_signature_now(&author, "tgit2", "tgit2@localhost") == 0 &&
        git_commit_create(&id, bare, "HEAD", author, author, NULL, "Initial commit", tree, 0, NULL) == 0) {
        oid_result(c->interp, &id);
        rc = 0;
    }
    git_signature_free(author);
    git_tree_free(tree);
    git_treebuilder_free(builder);
    git_repository_free(bare);
    if (rc < 0) Tcl_FSRemoveDirectory(c->obj[0], 1, NULL);
    return rc;
}

static int private_branch(Tcl_Interp *interp, Tcl_DString *branch, const char *name)
{
    int valid;
    if (*name == 0 || strpbrk(name, "/\\") || git_branch_name_is_valid(&valid, name) < 0 || !valid)
        return invalid(interp, "expected a single valid worktree name");
    Tcl_DStringAppend(branch, "tgit2-worktrees/", -1);
    Tcl_DStringAppend(branch, name, -1);
    return 0;
}

static int add_worktree(Call *c)
{
    git_oid id;
    git_commit *base = NULL;
    git_reference *branch = NULL;
    git_worktree *wt = NULL;
    git_worktree_add_options opts = GIT_WORKTREE_ADD_OPTIONS_INIT;
    Tcl_StatBuf *st = Tcl_AllocStatBuf();
    Tcl_DString name;
    int rc = -1;
    Tcl_DStringInit(&name);
    if (Tcl_FSLstat(c->obj[2], st) == 0) {
        invalid(c->interp, "worktree destination already exists");
        goto done;
    }
    if (private_branch(c->interp, &name, c->arg[1]) < 0 ||
        parse_oid(c->interp, &id, c->arg[3]) < 0 ||
        git_commit_lookup(&base, c->repo, &id) < 0 ||
        git_branch_create(&branch, c->repo, Tcl_DStringValue(&name), base, 0) < 0)
        goto done;
    opts.ref = branch;
    rc = git_worktree_add(&wt, c->repo, c->arg[1], c->arg[2], &opts);
    if (rc < 0) {
        const git_error *err = git_error_last();
        Tcl_SetObjResult(c->interp, Tcl_NewStringObj(err ? err->message : "worktree creation failed", -1));
        git_branch_delete(branch);
    }
done:
    Tcl_Free((char *)st);
    Tcl_DStringFree(&name);
    git_worktree_free(wt);
    git_reference_free(branch);
    git_commit_free(base);
    return rc;
}

static int list_worktrees(Call *c)
{
    git_strarray names = {0};
    Tcl_Obj *list;
    if (git_worktree_list(&names, c->repo) < 0) return -1;
    list = Tcl_NewListObj(0, NULL);
    for (size_t i = 0; i < names.count; ++i)
        Tcl_ListObjAppendElement(c->interp, list, Tcl_NewStringObj(names.strings[i], -1));
    Tcl_SetObjResult(c->interp, list);
    git_strarray_dispose(&names);
    return 0;
}

static int prune_worktree(Call *c, int remove_files)
{
    git_worktree_prune_options opts = GIT_WORKTREE_PRUNE_OPTIONS_INIT;
    git_reference *branch = NULL;
    git_worktree *wt = NULL;
    Tcl_DString name;
    int rc = -1;
    Tcl_DStringInit(&name);
    if (private_branch(c->interp, &name, c->arg[1]) < 0 ||
        git_worktree_lookup(&wt, c->repo, c->arg[1]) < 0 ||
        git_branch_lookup(&branch, c->repo, Tcl_DStringValue(&name), GIT_BRANCH_LOCAL) < 0)
        goto done;
    if (remove_files) opts.flags = GIT_WORKTREE_PRUNE_VALID | GIT_WORKTREE_PRUNE_WORKING_TREE;
    if (git_worktree_prune(wt, &opts) == 0) rc = git_branch_delete(branch);
done:
    Tcl_DStringFree(&name);
    git_reference_free(branch);
    git_worktree_free(wt);
    return rc;
}

static int remove_worktree(Call *c) { return prune_worktree(c, 1); }
static int prune_missing(Call *c) { return prune_worktree(c, 0); }

static int get_ref(Call *c)
{
    git_oid id;
    if (git_reference_name_to_id(&id, c->repo, c->arg[1]) < 0) return -1;
    oid_result(c->interp, &id);
    return 0;
}

static int update_ref(Call *c)
{
    git_oid expected, next;
    git_commit *commit = NULL;
    git_reference *ref = NULL;
    int rc;
    if (parse_oid(c->interp, &expected, c->arg[2]) < 0 ||
        parse_oid(c->interp, &next, c->arg[3]) < 0 ||
        git_commit_lookup(&commit, c->repo, &next) < 0) return -1;
    rc = git_reference_create_matching(&ref, c->repo, c->arg[1], &next, 1, &expected, "Update reference");
    git_reference_free(ref);
    git_commit_free(commit);
    return rc;
}

static int stage_paths(Call *c, git_index *index)
{
    Tcl_Obj **items;
    TgitSize count;
    if (Tcl_ListObjGetElements(c->interp, c->obj[1], &count, &items) != TCL_OK) return -1;
    for (TgitSize i = 0; i < count; i++) {
        Tcl_DString buf;
        int rc;
        Tcl_DStringInit(&buf);
        rc = external_string(c->interp, c->utf8, items[i], &buf);
        if (rc == 0) {
            const char *path = Tcl_DStringValue(&buf);
            rc = git_index_add_bypath(index, path);
            if (rc == GIT_ENOTFOUND) rc = git_index_remove_bypath(index, path);
        }
        Tcl_DStringFree(&buf);
        if (rc < 0) return rc;
    }
    return 0;
}

static int stage(Call *c)
{
    git_index *index = NULL;
    int rc = git_repository_index(&index, c->repo);
    if (!rc) rc = stage_paths(c, index);
    if (!rc) rc = git_index_write(index);
    git_index_free(index);
    return rc;
}

static int commit(Call *c)
{
    git_index *index = NULL;
    git_tree *tree = NULL;
    git_commit *base = NULL;
    const git_commit *parents[1];
    git_signature *author = NULL;
    git_oid tree_id, id;
    int rc = -1;

    if (git_reference_name_to_id(&id, c->repo, "HEAD") < 0 ||
        git_commit_lookup(&base, c->repo, &id) < 0 ||
        git_repository_index(&index, c->repo) < 0 ||
        stage_paths(c, index) < 0 ||
        git_index_write_tree(&tree_id, index) < 0 ||
        git_index_write(index) < 0) goto done;
    if (!git_oid_equal(&tree_id, git_commit_tree_id(base))) {
        parents[0] = base;
        if (git_tree_lookup(&tree, c->repo, &tree_id) < 0 ||
            git_signature_now(&author, "tgit2", "tgit2@localhost") < 0 ||
            git_commit_create(&id, c->repo, "HEAD", author, author, NULL,
                              c->arg[2], tree, 1, parents) < 0) goto done;
    }
    oid_result(c->interp, &id);
    rc = 0;
done:
    git_signature_free(author);
    git_commit_free(base);
    git_tree_free(tree);
    git_index_free(index);
    return rc;
}

static int diff(Call *c)
{
    git_tree *tree = NULL;
    git_commit *head = NULL;
    git_oid id;
    git_diff *changes = NULL;
    git_buf patch = GIT_BUF_INIT;
    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    int rc = -1;
    opts.flags = GIT_DIFF_INCLUDE_UNTRACKED | GIT_DIFF_RECURSE_UNTRACKED_DIRS | GIT_DIFF_SHOW_UNTRACKED_CONTENT;
    if (git_reference_name_to_id(&id, c->repo, "HEAD") == 0 &&
        git_commit_lookup(&head, c->repo, &id) == 0 &&
        git_commit_tree(&tree, head) == 0 &&
        git_diff_tree_to_workdir_with_index(&changes, c->repo, tree, &opts) == 0 &&
        git_diff_to_buf(&patch, changes, GIT_DIFF_FORMAT_PATCH) == 0) {
        if (patch.size > INT_MAX) rc = invalid(c->interp, "diff exceeds Tcl string limit");
        else {
            Tcl_SetObjResult(c->interp, Tcl_NewStringObj(patch.ptr, (int)patch.size));
            rc = 0;
        }
    }
    git_buf_dispose(&patch);
    git_diff_free(changes);
    git_commit_free(head);
    git_tree_free(tree);
    return rc;
}

static int status(Call *c)
{
    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    git_status_list *status_list = NULL;
    Tcl_Obj *result;
    opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
    if (git_status_list_new(&status_list, c->repo, &opts) < 0) return -1;
    result = Tcl_NewDictObj();
    for (size_t i = 0; i < git_status_list_entrycount(status_list); i++) {
        const git_status_entry *entry = git_status_byindex(status_list, i);
        const git_diff_delta *delta = entry->index_to_workdir ? entry->index_to_workdir : entry->head_to_index;
        Tcl_DictObjPut(c->interp, result, Tcl_NewStringObj(delta->new_file.path, -1),
                       Tcl_NewWideIntObj(entry->status));
    }
    Tcl_SetObjResult(c->interp, result);
    git_status_list_free(status_list);
    return 0;
}

/* Select exactly the commits, trees, and blobs reachable from the requested tip. */
static int reachable_pack(Call *c, git_oid *id, git_packbuilder **pack)
{
    git_revwalk *walk = NULL;
    int rc = -1;
    if (parse_oid(c->interp, id, c->arg[1]) == 0 &&
        git_revwalk_new(&walk, c->repo) == 0 &&
        git_revwalk_push(walk, id) == 0 &&
        git_packbuilder_new(pack, c->repo) == 0)
        rc = git_packbuilder_insert_walk(*pack, walk);
    git_revwalk_free(walk);
    return rc;
}

static int verify(Call *c)
{
    git_packbuilder *pack = NULL;
    git_oid id;
    int rc = reachable_pack(c, &id, &pack);
    if (rc == 0) oid_result(c->interp, &id);
    git_packbuilder_free(pack);
    return rc;
}

static int export_bare(Call *c)
{
    git_reference *main_ref = NULL;
    git_repository *target = NULL;
    git_packbuilder *pack = NULL;
    git_oid id;
    Tcl_DString dir;
    int created = 0, rc = -1;
    Tcl_DStringInit(&dir);
    if (reachable_pack(c, &id, &pack) < 0) goto done;
    if (Tcl_FSCreateDirectory(c->obj[2]) != TCL_OK) {
        invalid(c->interp, "could not create a fresh export directory");
        goto done;
    }
    created = 1;
    Tcl_DStringAppend(&dir, c->arg[2], -1);
    Tcl_DStringAppend(&dir, "/objects/pack", -1);
    if (create_bare(&target, c->arg[2]) == 0 &&
        git_packbuilder_write(pack, Tcl_DStringValue(&dir), 0, NULL, NULL) == 0 &&
        git_reference_create(&main_ref, target, "refs/heads/main", &id, 0, NULL) == 0) {
        oid_result(c->interp, &id);
        rc = 0;
    }
done:
    Tcl_DStringFree(&dir);
    git_packbuilder_free(pack);
    git_reference_free(main_ref);
    git_repository_free(target);
    if (rc < 0 && created) Tcl_FSRemoveDirectory(c->obj[2], 1, NULL);
    return rc;
}

typedef struct {
    const char *name;
    const char *usage;
    int count;
    int (*function)(Call *c);
} Command;

static int invoke(const Command *cmd, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    Tcl_DString buffers[4];
    Call c = {0};
    int converted = 0, rc = -1;
    if (objc != cmd->count + 1) {
        Tcl_WrongNumArgs(interp, 1, objv, cmd->usage);
        return TCL_ERROR;
    }
    c.interp = interp;
    c.obj = objv + 1;
    c.utf8 = Tcl_GetEncoding(interp, "utf-8");
    if (!c.utf8) return TCL_ERROR;
    for (int i = 0; i < cmd->count; i++) {
        Tcl_DStringInit(&buffers[i]);
        converted++;
        Tcl_Obj *value = c.obj[i];
        if (i == 0 || (i == 2 && (cmd->function == add_worktree || cmd->function == export_bare))) {
            value = Tcl_FSGetNormalizedPath(interp, value);
            if (!value || !Tcl_FSGetNativePath(value)) {
                invalid(interp, "expected a native filesystem path");
                goto done;
            }
        }
        if (external_string(interp, c.utf8, value, &buffers[i]) < 0) goto done;
        c.arg[i] = Tcl_DStringValue(&buffers[i]);
    }
    if (cmd->function != init_repo &&
        git_repository_open_ext(&c.repo, c.arg[0], GIT_REPOSITORY_OPEN_NO_SEARCH, NULL) < 0)
        goto done;
    rc = cmd->function(&c);
done:
    if (rc < 0) {
        if (Tcl_GetStringResult(interp)[0] == 0) {
            const git_error *err = git_error_last();
            Tcl_SetObjResult(interp, Tcl_NewStringObj(err ? err->message : "Git operation failed", -1));
        }
        Tcl_SetErrorCode(interp, "TGIT2", cmd->name, NULL);
    }
    git_repository_free(c.repo);
    for (int i = 0; i < converted; i++) Tcl_DStringFree(&buffers[i]);
    Tcl_FreeEncoding(c.utf8);
    return rc < 0 ? TCL_ERROR : TCL_OK;
}

static int simple_cmd(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    return invoke(data, interp, objc, objv);
}

static int subcommand(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    const Command *commands = data;
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "subcommand ?arg ...?");
        return TCL_ERROR;
    }
    for (const Command *cmd = commands; cmd->name; ++cmd)
        if (strcmp(cmd->name, Tcl_GetString(objv[1])) == 0)
            return invoke(cmd, interp, objc - 1, objv + 1);
    Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown subcommand: %s", Tcl_GetString(objv[1])));
    return TCL_ERROR;
}

void tgit2_register_workspace(Tcl_Interp *interp)
{
    static const Command commands[] = {
        {"::tgit2::init", "directory", 1, init_repo},
        {"::tgit2::stage", "worktree paths", 2, stage},
        {"::tgit2::commit", "worktree paths message", 3, commit},
        {"::tgit2::status", "worktree", 1, status},
        {"::tgit2::diff", "worktree", 1, diff},
        {"::tgit2::verify", "repository commitId", 2, verify},
        {"::tgit2::export_bare", "repository commitId directory", 3, export_bare},
        {NULL, NULL, 0, NULL}
    };
    static const Command worktrees[] = {
        {"add", "repository name directory commitId", 4, add_worktree},
        {"list", "repository", 1, list_worktrees},
        {"remove", "repository name", 2, remove_worktree},
        {"prune", "repository name", 2, prune_missing},
        {NULL, NULL, 0, NULL}
    };
    static const Command refs[] = {
        {"get", "repository refName", 2, get_ref},
        {"update", "repository refName expectedId newId", 4, update_ref},
        {NULL, NULL, 0, NULL}
    };
    for (const Command *cmd = commands; cmd->name; ++cmd)
        Tcl_CreateObjCommand(interp, cmd->name, simple_cmd, (ClientData)cmd, NULL);
    Tcl_CreateObjCommand(interp, "::tgit2::worktree", subcommand, (ClientData)worktrees, NULL);
    Tcl_CreateObjCommand(interp, "::tgit2::ref", subcommand, (ClientData)refs, NULL);
}
