#pragma once

#include "Common.hpp"
#include "Hash.hpp"



template <typename EnumType, typename StorageType>
class FrozenEnum
{
private:
    StorageType m_Value;

public:
    operator EnumType() const { return static_cast<EnumType>(m_Value); }

private:
    FrozenEnum();
    ~FrozenEnum();
    FrozenEnum &operator=(const FrozenEnum &);
    FrozenEnum(const FrozenEnum &);
};

template <typename T>
class FrozenPtr
{
private:
    int32_t m_Offset;

public:
    const T *Get() const
    {
        uintptr_t base = (uintptr_t)this;
        uintptr_t target = base + m_Offset;
        uintptr_t null_mask = (0 == m_Offset) ? 0 : ~uintptr_t(0);
        return reinterpret_cast<T *>(target & null_mask);
    }

    operator const T *() const { return Get(); }

private:
    FrozenPtr();
    ~FrozenPtr();
    FrozenPtr &operator=(const FrozenPtr &);
    FrozenPtr(const FrozenPtr &);
};

typedef FrozenPtr<const char> FrozenString;

const uint64_t storage_for_empty_frozenarrays = 0;

template <typename T>
class FrozenArray
{
private:
    int32_t m_Count;
    FrozenPtr<T> m_Pointer;

public:
    int32_t GetCount() const { return m_Count; }
    const T *GetArray() const { return m_Pointer; }

    const T *begin() const { return m_Pointer; }
    const T *end() const { return m_Pointer + m_Count; }

    const T &operator[](int32_t index) const
    {
        CHECK(uint32_t(index) < uint32_t(m_Count));
        return m_Pointer[index];
    }

    static const FrozenArray<T> &empty()
    {
        return *reinterpret_cast<const FrozenArray<T> *>(&storage_for_empty_frozenarrays);
    }

private:
    FrozenArray();
    ~FrozenArray();
    FrozenArray &operator=(const FrozenArray &);
    FrozenArray(const FrozenArray &);
};

struct FrozenFileAndHash
{
    FrozenString m_Filename;
    uint32_t m_FilenameHash;
};
