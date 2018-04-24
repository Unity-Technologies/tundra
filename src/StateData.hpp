#ifndef STATEDATA_HPP
#define STATEDATA_HPP

#include "Common.hpp"
#include "BinaryData.hpp"

namespace t2
{

struct NodeSignatureComponent
{
  FrozenString m_Key;
  FrozenString m_Value;
};

struct NodeStateData
{
  int32_t                   m_BuildResult;
  HashDigest                m_InputSignature;
  FrozenArray<FrozenString> m_OutputFiles;
  FrozenArray<FrozenString> m_AuxOutputFiles;
  uint32_t                  m_TimeStampOfLastUseInDays;
  FrozenArray<NodeSignatureComponent> m_InputSignatureComponents;
};

struct StateData
{
  static const uint32_t     MagicNumber = 0x15890103 ^ kTundraHashMagic;

  uint32_t                 m_MagicNumber;

  int32_t                  m_NodeCount;
  FrozenPtr<HashDigest>    m_NodeGuids;
  FrozenPtr<NodeStateData> m_NodeStates;

  uint32_t                   m_MagicNumberEnd;
};

}

#endif
