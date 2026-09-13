/***************************************************************************
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU General Public                   *
 *   License as published by the Free Software Foundation;                 *
 *   version 2 of the License.                                             *
 *                                                                         *
 *   As a special exception, you may use this file as part of a free       *
 *   software library without restriction.  Specifically, if other files   *
 *   instantiate templates or use macros or inline functions from this     *
 *   file, or you compile this file and link it with other files to        *
 *   produce an executable, this file does not by itself cause the         *
 *   resulting executable to be covered by the GNU General Public          *
 *   License.  This exception does not however invalidate any other        *
 *   reasons why the executable file might be covered by the GNU General   *
 *   Public License.                                                       *
 *                                                                         *
 *   This library is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU     *
 *   Lesser General Public License for more details.                       *
 *                                                                         *
 *   You should have received a copy of the GNU General Public             *
 *   License along with this library; if not, write to the Free Software   *
 *   Foundation, Inc., 59 Temple Place,                                    *
 *   Suite 330, Boston, MA  02111-1307  USA                                *
 *                                                                         *
 ***************************************************************************/


#ifndef ORO_BUFFER_POLICY_HPP
#define ORO_BUFFER_POLICY_HPP

#include "rtt-config.h"
#include <iosfwd>

namespace RTT {

    /**
     * Storage placement for latest-value DATA connections. The historical name
     * does not enable FIFO data ports. TaskContext-owned cyclic inputs accept
     * only one whole writer; unregistered transport endpoints may have several.
     *
     * - PerConnection: each port pair/stream owns its data object. An unregistered
     *   input with several writers checks its previous source before other sources.
     * - PerInputPort: one data object is shared by the input's sources. Remote
     *   input streams require pull == false.
     * - PerOutputPort: readers share the output's data object. Remote output
     *   streams require pull == true.
     * - Shared: ports share one data object. At most one writer may feed the group
     *   if it contains a TaskContext-owned input. Several readers can attach.
     *
     * Cyclic inputs track freshness independently, including shared storage:
     * preparing one input does not consume another input's next NewData update.
     * Low-level channel reads retain their shared freshness semantics.
     *
     * @ingroup Ports
     */
    typedef enum {
        UnspecifiedBufferPolicy,
        PerConnection,
        PerInputPort,
        PerOutputPort,
        Shared,
    } BufferPolicy;

    RTT_API std::ostream &operator<<(std::ostream &os, const BufferPolicy &bp);
    RTT_API std::istream &operator>>(std::istream &is, BufferPolicy &bp);
}

#endif
