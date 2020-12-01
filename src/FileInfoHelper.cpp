#include "FileInfoHelper.hpp"

Frozen::DagStatSignature::Enum GetStatSignatureStatusFor(const FileInfo& fileInfo)
{
    return fileInfo.IsFile()
            ? Frozen::DagStatSignature::Enum::File
            : fileInfo.IsDirectory()
                ? Frozen::DagStatSignature::Enum::Directory
                : Frozen::DagStatSignature::Enum::DoesNotExist;
}
