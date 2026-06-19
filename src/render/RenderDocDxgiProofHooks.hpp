#pragma once

namespace uevr::renderdoc_dxgi_proof {

bool install();

class ScopedInternalFactoryProof {
public:
    ScopedInternalFactoryProof();
    ~ScopedInternalFactoryProof();

    ScopedInternalFactoryProof(const ScopedInternalFactoryProof&) = delete;
    ScopedInternalFactoryProof& operator=(const ScopedInternalFactoryProof&) = delete;
};

} // namespace uevr::renderdoc_dxgi_proof
