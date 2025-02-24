#ifndef UNSTABLE_ERASE_HH
#define UNSTABLE_ERASE_HH

namespace util {

template <typename Container>
void unstable_erase(Container& container, typename Container::size_type pos)
{
  container[pos] = std::move(container.back());
  container.pop_back();
}

template <typename Container>
void unstable_erase(Container& container, typename Container::iterator itr)
{
  *itr = std::move(container.back());
  container.pop_back();
}

}

#endif //UNSTABLE_ERASE_HH
