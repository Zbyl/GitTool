
#include <iostream>
#include <string>

#include <boost/filesystem.hpp>
#include <boost/exception/all.hpp>
#include <boost/scope_exit.hpp>

#include <git2.h>

template<typename F>
class ScopeExit
{
public:
    ScopeExit(F&& f)
        : f(f)
    {}

    ~ScopeExit()
    {
        f();
    }
private:
    F f;
};
// ScopeExit scopeExit ## __LINE__(functor)

#define SCOPE_EXIT(functor) \
    const auto scDeleter ## __LINE__ = [=]() functor; \
    const auto scGuard ## __LINE__ = std::unique_ptr<char*, decltype(scDeleter ## __LINE__)>(nullptr, std::move(scDeleter ## __LINE__))

void logOnGitError(int error, const std::string& message)
{
    if (error < 0)
    {
      const git_error *e = giterr_last();
      std::cout << "ERROR: " << message << " (" << error  << "/" << e->klass << ": " << e->message << ")." << std::endl;
    }
}

void throwOnGitError(int error, const std::string& message)
{
    if (error < 0)
    {
      logOnGitError(error, message);
      throw "Too bad.";
    }
}

git_repository* init_repo(const boost::filesystem::path& repoPath)
{
    int error;

    git_repository *repo = NULL;
    error = git_repository_init(&repo, repoPath.string().c_str(), 0);
    throwOnGitError(error, "Could not initialize repository.");

    const char* repoWorkDir = git_repository_workdir(repo);
    std::cout << "Initialized empty Git repository in: " << repoWorkDir << std::endl;

    return repo;
}

git_repository* open_repo(const boost::filesystem::path& repoPath)
{
    int error;

    git_repository *repo = NULL;
    error = git_repository_open(&repo, repoPath.string().c_str());
    throwOnGitError(error, "Could not open repository.");

    const char* repoWorkDir = git_repository_workdir(repo);
    std::cout << "Opened Git repository in: " << repoWorkDir << std::endl;

    return repo;
}

static git_oid create_initial_commit(git_repository* repo)
{
    int error;

    git_signature *sig;
    error = git_signature_default(&sig, repo);
    throwOnGitError(error, "Unable to create a commit signature. Perhaps 'user.name' and 'user.email' are not set");
    BOOST_SCOPE_EXIT_ALL(=) { git_signature_free(sig); };

    git_index *index;
    error = git_repository_index(&index, repo);
    throwOnGitError(error, "Could not open repository index.");
    BOOST_SCOPE_EXIT_ALL(=) { git_index_free(index); };

    /// All this code does is use the empty index to get the SHA-1 hash of the empty tree.
    git_oid tree_id;
    error = git_index_write_tree(&tree_id, index);
    throwOnGitError(error, "Unable to write initial tree from index.");
    BOOST_SCOPE_EXIT_ALL(=) { git_index_free(index); };

    git_tree *tree;
    error = git_tree_lookup(&tree, repo, &tree_id);
    throwOnGitError(error, "Could not look up initial tree.");
    BOOST_SCOPE_EXIT_ALL(=) { git_tree_free(tree); };

    git_oid commit_id;
    error = git_commit_create(
            &commit_id, repo, "HEAD",
            sig,    // author
            sig,    // commiter
            NULL,   // encoding: deafults to UTF-8
            "Initial commit",
            tree,
            0,      // parent commits count
            NULL);  // no parent commits
    throwOnGitError(error, "Could not create the initial commit.");

    std::cout << "Created empty initial commit." << std::endl;

    return commit_id;
}

int print_cb(
    const git_diff_delta *delta, /**< delta that contains this data */
    const git_diff_hunk *hunk,   /**< hunk containing this data */
    const git_diff_line *line,   /**< line data */
    void *payload)              /**< user reference data */
{
    std::string sline(line->content, line->content + line->content_len);
    std::cout << sline << std::endl;

    return 0;
}

void dumpDiff(git_repository* repo, git_commit *commit0, git_commit *commit1)
{
    int error;

    git_tree *commit0_tree = NULL;
    error = git_commit_tree(&commit0_tree, commit0);
    throwOnGitError(error, "Could not create commit0 tree.");
    BOOST_SCOPE_EXIT_ALL(=) { git_tree_free(commit0_tree); };

    git_tree *commit1_tree = NULL;
    error = git_commit_tree(&commit1_tree, commit1);
    throwOnGitError(error, "Could not create commit1 tree.");
    BOOST_SCOPE_EXIT_ALL(=) { git_tree_free(commit1_tree); };

    git_diff *diff = NULL;
    error = git_diff_tree_to_tree(&diff, repo, commit0_tree, commit1_tree, NULL);
    throwOnGitError(error, "Could not create diff.");
    BOOST_SCOPE_EXIT_ALL(= ) { git_diff_free(diff); };

    error = git_diff_print(diff, GIT_DIFF_FORMAT_PATCH, print_cb, NULL);
    throwOnGitError(error, "Could not print diff.");
}

void findAllOnMaster(git_repository* repo)
{
    int error;

#if 0
    git_oid commit_id;
    error = reference_name_to_id(&commit_id, repo, "refs/head/master");
#endif

    git_object *obj = NULL;
    error = git_revparse_single(&obj, repo, "master");
    throwOnGitError(error, "Revparse failed.");
    BOOST_SCOPE_EXIT_ALL(=) { git_object_free(obj); };

    git_commit *commit = NULL;
    error = git_commit_lookup(&commit, repo, git_object_id(obj));
    throwOnGitError(error, "Initial commit lookup failed.");
    BOOST_SCOPE_EXIT_ALL(&) { git_commit_free(commit); };

    while (commit)
    {
        const git_oid* commitId = git_commit_id(commit);
        char commidIdString[GIT_OID_HEXSZ + 1];
        char* printableString = git_oid_tostr(commidIdString, sizeof(commidIdString), commitId);

        unsigned int parentCount = git_commit_parentcount(commit);

        std::cout << "Commit " << printableString << " has " << parentCount << " parents." << std::endl;

        if (parentCount == 0)
            break;

        const git_oid* firstParentId = git_commit_parent_id(commit, 0);
        git_commit *parentCommit = NULL;
        error = git_commit_lookup(&parentCommit, repo, firstParentId);
        throwOnGitError(error, "Parent commit lookup failed.");
        BOOST_SCOPE_EXIT_ALL(&) { git_commit_free(parentCommit); };

        dumpDiff(repo, parentCommit, commit);

        std::swap(commit, parentCommit);
    }

    std::cout << "Done." << std::endl;

#if 0
    git_reference *ref = NULL;
    error = git_reference_dwim(&ref, repo, "master");
    throwOnGitError(error, "Getting commit failed.");
    BOOST_SCOPE_EXIT_ALL(=) { git_reference_free(ref); };
#endif
}

int main()
{
    try
    {
        int error;

        error = git_libgit2_init();
        throwOnGitError(error, "Git initialization failed.");
        BOOST_SCOPE_EXIT_ALL(=) { int error = git_libgit2_shutdown(); logOnGitError(error, "Git shutdown failed."); };

        boost::filesystem::path repoPath = boost::filesystem::current_path();
        repoPath /= "../GitToolPlayground";
        boost::filesystem::create_directories(repoPath);

        //git_repository* repo = init_repo(repoPath);
        git_repository* repo = open_repo(repoPath);
        BOOST_SCOPE_EXIT_ALL(=) { git_repository_free(repo); };

        //create_initial_commit(repo);
        findAllOnMaster(repo);
    }
    catch (...)
    {
        std::cout << "Exception caught: " << boost::current_exception_diagnostic_information();
    }

    return 0;
}
