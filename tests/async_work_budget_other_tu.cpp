#include "online/async_work_budget.h"

AsyncWorkBudget &resolverBudgetFromOtherTranslationUnit() {
    return onlineResolverWorkBudget();
}

AsyncWorkBudget::Permit acquireResolverPermitFromOtherTranslationUnit() {
    return onlineResolverWorkBudget().tryAcquire();
}

std::size_t observeResolverWorkFromOtherTranslationUnit() noexcept {
    return onlineResolverWorkInUse();
}
