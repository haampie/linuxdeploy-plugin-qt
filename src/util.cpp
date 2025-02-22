// local headers
#include "util.h"

procOutput check_command(const std::vector<std::string> &args) {
    auto command = subprocess::util::join(args);
    subprocess::Popen proc(command, subprocess::bufsize{-1 /* stands for dynamically allocated buffer */},
                           subprocess::output(subprocess::PIPE), subprocess::error(subprocess::PIPE));
    auto outputs = proc.communicate();

    const auto &outBuf = outputs.first.buf;
    auto outBufEnd = std::find(outBuf.begin(), outBuf.end(), '\0');
    std::string out(outBuf.begin(), outBufEnd);

    const auto &errBuf = outputs.second.buf;
    auto errBufEnd = std::find(errBuf.begin(), errBuf.end(), '\0');
    std::string err(errBuf.begin(),  errBufEnd);

    int returnCode = proc.retcode();
    return {returnCode == 0, returnCode, out, err};
}

boost::filesystem::path which(const std::string &name) {
    subprocess::Popen proc({"which", name.c_str()}, subprocess::output(subprocess::PIPE));
    auto output = proc.communicate();

    using namespace linuxdeploy::core::log;

    ldLog() << LD_DEBUG << "Calling 'which" << name << LD_NO_SPACE << "'" << std::endl;

    if (proc.retcode() != 0) {
        ldLog() << LD_DEBUG << "which call failed, exit code:" << proc.retcode() << std::endl;
        return "";
    }

    std::string path = output.first.buf.data();

    while (path.back() == '\n') {
        path.erase(path.end() - 1, path.end());
    }

    return path;
}

std::map<std::string, std::string> queryQmake(const boost::filesystem::path& qmakePath) {
    auto qmakeCall = check_command({qmakePath.string(), "-query"});

    using namespace linuxdeploy::core::log;

    if (!qmakeCall.success) {
        ldLog() << LD_ERROR << "Call to qmake failed:" << qmakeCall.stderrOutput << std::endl;
        return {};
    }

    std::map<std::string, std::string> rv;

    std::stringstream ss;
    ss << qmakeCall.stdoutOutput;

    std::string line;

    auto stringSplit = [](const std::string& str, const char delim = ' ') {
        std::stringstream ss;
        ss << str;

        std::string part;
        std::vector<std::string> parts;

        while (std::getline(ss, part, delim)) {
            parts.push_back(part);
        }

        return parts;
    };

    while (std::getline(ss, line)) {
        auto parts = stringSplit(line, ':');

        if (parts.size() != 2)
            continue;

        rv[parts[0]] = parts[1];
    }

    return rv;
};

boost::filesystem::path findQmake() {
    using namespace linuxdeploy::core::log;

    boost::filesystem::path qmakePath;

    // allow user to specify absolute path to qmake
    if (getenv("QMAKE")) {
        qmakePath = getenv("QMAKE");
        ldLog() << "Using user specified qmake:" << qmakePath << std::endl;
    } else {
        // search for qmake
        qmakePath = which("qmake-qt5");

        if (qmakePath.empty())
            qmakePath = which("qmake");
    }

    return qmakePath;
}

bool pathContainsFile(boost::filesystem::path dir, boost::filesystem::path file) {
    // If dir ends with "/" and isn't the root directory, then the final
    // component returned by iterators will include "." and will interfere
    // with the std::equal check below, so we strip it before proceeding.
    if (dir.filename() == ".")
        dir.remove_filename();
    // We're also not interested in the file's name.
    assert(file.has_filename());
    file.remove_filename();

    // If dir has more components than file, then file can't possibly
    // reside in dir.
    auto dir_len = std::distance(dir.begin(), dir.end());
    auto file_len = std::distance(file.begin(), file.end());
    if (dir_len > file_len)
        return false;

    // This stops checking when it reaches dir.end(), so it's OK if file
    // has more directory components afterward. They won't be checked.
    return std::equal(dir.begin(), dir.end(), file.begin());
};

std::string join(const std::vector<std::string> &list) {
    return join(list.begin(), list.end());
}

std::string join(const std::set<std::string> &list) {
    return join(list.begin(), list.end());
}

bool strStartsWith(const std::string &str, const std::string &prefix) {
    if (str.size() < prefix.size())
        return false;

    return strncmp(str.c_str(), prefix.c_str(), prefix.size()) == 0;
}

bool strEndsWith(const std::string &str, const std::string &suffix) {
    if (str.size() < suffix.size())
        return false;

    return strncmp(str.c_str() + (str.size() - suffix.size()), suffix.c_str(), suffix.size()) == 0;
}

