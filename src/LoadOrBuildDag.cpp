#include "stdarg.h"
#include "Driver.hpp"
#include "NodeResultPrinting.hpp"
#include "DagGenerator.hpp"
#include "LoadFrozenData.hpp"
#include "Profiler.hpp"
#include "FileSign.hpp"
#include "DagDerivedCompiler.hpp"
#include "FileInfoHelper.hpp"
#include "DetectCyclicDependencies.hpp"

static bool ExitRequestingFrontendRun(const char *reason_fmt, ...)
{
    char buffer[1024];

    va_list args;
    va_start(args, reason_fmt);
    int written =vsnprintf(buffer,1024,reason_fmt, args);
    va_end(args);
    buffer[written] = 0;


    PrintMessage(MessageStatusLevel::Success, "Require frontend run.  %s", buffer);

    exit(BuildResult::kRequireFrontendRerun);
    return false;
}


static bool DriverCheckDagSignatures(Driver *self, char *out_of_date_reason, int out_of_date_reason_maxlength)
{
    const Frozen::Dag *dag_data = self->m_DagData;

#if ENABLED(CHECKED_BUILD)
    // Paranoia - make sure the data is sorted.
    for (int i = 1, count = dag_data->m_NodeCount; i < count; ++i)
    {
        if (dag_data->m_NodeGuids[i] < dag_data->m_NodeGuids[i - 1])
            Croak("DAG data is not sorted by guid");
    }
#endif

    Log(kDebug, "checking file signatures for DAG data");

    // Check timestamps of frontend files used to produce the DAG
    for (const Frozen::DagFileSignature &sig : dag_data->m_FileSignatures)
    {
        const char *path = sig.m_Path;

        uint64_t timestamp = sig.m_Timestamp;
        FileInfo info = GetFileInfo(path);

        if (info.m_Timestamp != timestamp)
        {
            snprintf(out_of_date_reason, out_of_date_reason_maxlength, "FileSignature timestamp changed: %s", sig.m_Path.Get());
            return false;
        }
    }

    for (const Frozen::DagStatSignature &sig : dag_data->m_StatSignatures)
    {
        const char *path = sig.m_Path;
        FileInfo info = GetFileInfo(path);

        if (GetStatSignatureStatusFor(info) != sig.m_StatResult)
        {
            snprintf(out_of_date_reason, out_of_date_reason_maxlength, "StatSignature changed: %s", sig.m_Path.Get());
            return false;
        }
    }


    // Check directory listing fingerprints
    // Note that the digest computation in here must match the one in LuaListDirectory
    // The digests computed there are stored in the signature block by frontend code.
    for (const Frozen::DagGlobSignature &sig : dag_data->m_GlobSignatures)
    {
        HashDigest digest = CalculateGlobSignatureFor(sig.m_Path, sig.m_Filter, sig.m_Recurse, &self->m_Heap, &self->m_Allocator);

        // Compare digest with the one stored in the signature block
        if (0 != memcmp(&digest, &sig.m_Digest, sizeof digest))
        {
            char stored[kDigestStringSize], actual[kDigestStringSize];
            DigestToString(stored, sig.m_Digest);
            DigestToString(actual, digest);
            snprintf(out_of_date_reason, out_of_date_reason_maxlength, "directory contents changed: %s", sig.m_Path.Get());
            Log(kInfo, "DAG out of date: file glob change for %s (%s => %s)", sig.m_Path.Get(), stored, actual);
            return false;
        }
    }

    return true;
}

bool LoadOrBuildDag(Driver *self, const char *dag_fn)
{
    const int out_of_date_reason_length = 500;
    char out_of_date_reason[out_of_date_reason_length + 1];

    snprintf(out_of_date_reason, out_of_date_reason_length, "(unknown reason)");

    char dagderived_filename[kMaxPathLength];
    snprintf(dagderived_filename, sizeof dagderived_filename, "%s_derived", dag_fn);
    dagderived_filename[sizeof(dagderived_filename) - 1] = '\0';

    FileInfo dagderived_info = GetFileInfo(dagderived_filename);

    if (self->m_Options.m_DagFileNameJson != nullptr)
    {
        if (!FreezeDagJson(self->m_Options.m_DagFileNameJson, dag_fn))
            return ExitRequestingFrontendRun("%s failed to freeze", self->m_Options.m_DagFileNameJson);
    }

    if (!LoadFrozenData<Frozen::Dag>(dag_fn, &self->m_DagFile, &self->m_DagData))
    {
        remove(dag_fn);
        remove(dagderived_filename);
        return ExitRequestingFrontendRun("%s couldn't be loaded", dag_fn);
    }

    if (DetectCyclicDependencies(self->m_DagData, &self->m_Heap))
    {
        MmapFileUnmap(&self->m_DagFile);
        remove(dag_fn);
        remove(dagderived_filename);
        exit(BuildResult::kBuildError);
        return 0;
    }

    if (!dagderived_info.Exists() || self->m_Options.m_DagFileNameJson != nullptr)
    {
        if (!CompileDagDerived(self->m_DagData, &self->m_Heap, &self->m_Allocator, &self->m_StatCache, dagderived_filename))
            return ExitRequestingFrontendRun("failed to create derived dag file %s", dagderived_filename);
    }

    if (!LoadFrozenData<Frozen::DagDerived>(dagderived_filename, &self->m_DagDerivedFile, &self->m_DagDerivedData))
    {
        remove(dag_fn);
        remove(dagderived_filename);
        return ExitRequestingFrontendRun("%s couldn't be loaded", dag_fn);
    }

    uint64_t time_exec_started = TimerGet();
    bool dagIsValid;
    {
        ProfilerScope prof_scope("DriverCheckDagSignatures", 0);
        dagIsValid = DriverCheckDagSignatures(self, out_of_date_reason, out_of_date_reason_length);
    }

    uint64_t now = TimerGet();
    double duration = TimerDiffSeconds(time_exec_started, now);
    if (duration > 1)
        PrintMessage(MessageStatusLevel::Warning, (int) duration, "Calculating file and glob signatures. (unusually slow)");

    if (dagIsValid)
        return true;

    if (self->m_Options.m_IncludesOutput != nullptr)
    {
        Log(kDebug, "Only showing includes; using existing DAG without out-of-date checks");
        return true;
    }

    MmapFileUnmap(&self->m_DagFile);
    self->m_DagData = nullptr;
    MmapFileUnmap(&self->m_DagDerivedFile);
    self->m_DagDerivedData = nullptr;

    if (remove(dag_fn))
        Croak("Failed to remove out of date dag at %s", dag_fn);
    if (remove(dagderived_filename))
        Croak("Failed to remove out of date dagderived file at %s", dagderived_filename);

    ExitRequestingFrontendRun("%s no longer valid. %s", FindFileNameInside(dag_fn), out_of_date_reason);

    return false;
}
